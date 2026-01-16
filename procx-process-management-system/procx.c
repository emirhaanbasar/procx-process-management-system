/*
 * ProcX - Gelişmiş Süreç Yönetim Sistemi
 *
 * Bu program, proje dokümanında tanımlanan "nohup benzeri" süreç yönetim
 * sistemini uygular. Birden fazla terminalden (instance) çalıştırıldığında
 * ortak bir shared memory üzerinde process listesi tutar, semaphore ile
 * senkronizasyon sağlar ve message queue ile instance'lar arası bildirim
 * gönderir.
 *
 * Özellikler (dokümana uygun):
 *  - fork() + execvp() ile process başlatma
 *  - Attached / Detached mod desteği (detached için setsid())
 *  - Shared memory üzerinde global process tablosu
 *  - Semaphore ile kritik bölge koruması
 *  - Message queue ile START / TERMINATED / KILLED olaylarının duyurulması
 *  - Monitor thread: waitpid(WNOHANG) ile çocukların sonlanmasını izler
 *  - IPC listener thread: diğer instance'lardan gelen mesajları ekrana yazar
 *  - Çıkışta attached process'leri öldürme, detached'leri bırakma
 *
 * Not: SIGCHLD yönetimi için handler eklendi. Monitor thread yine waitpid ile
 * çocukları "reap" eder (zombi önleme burada gerçekleşir).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------- IPC isimleri  ---------- */
#define SHM_NAME "/procx_shm"
#define SEM_NAME "/procx_sem"
#define MQ_NAME  "/procx_mq"

/* ---------- Sabitler ---------- */
#define MAX_PROCESSES 50
#define MAX_CMD_LEN   256
#define MQ_MAXMSG     10
#define MQ_MSGSIZE    sizeof(Message)
#define MONITOR_INTERVAL_SEC 2

/* ---------- Process modu ve durumu ---------- */
typedef enum {
    PROCESS_MODE_ATTACHED = 0,
    PROCESS_MODE_DETACHED = 1
} ProcessMode;

typedef enum {
    PROCESS_STATUS_RUNNING = 0,
    PROCESS_STATUS_TERMINATED = 1
} ProcessStatus;

/* ---------- Shared memory veri yapıları ---------- */
typedef struct {
    pid_t pid;
    pid_t owner_pid;
    char command[MAX_CMD_LEN];
    ProcessMode mode;
    ProcessStatus status;
    time_t start_time;
    int is_active;   // 1: aktif, 0: silinmiş/temizlenecek
} ProcessInfo;

typedef struct {
    ProcessInfo processes[MAX_PROCESSES];
    int process_count;   // kompakt edilmiş aktif kayıt sayısı
} SharedData;

/* ---------- Mesaj yapısı (message queue) ---------- */
typedef struct {
    long msg_type;       // POSIX mq’da kullanılmıyor ama şema için tutuluyor
    int command;         // 1 = START, 2 = TERMINATED, 3 = KILLED
    pid_t sender_pid;    // gönderen instance PID
    pid_t target_pid;    // etkilenen process PID
} Message;

typedef enum {
    MSG_PROCESS_STARTED    = 1,
    MSG_PROCESS_TERMINATED = 2,
    MSG_PROCESS_KILLED     = 3
} MessageCommand;

/* ---------- Global değişkenler ---------- */
static SharedData *g_shared = NULL;
static sem_t *g_sem = NULL;
static mqd_t g_mq = (mqd_t)-1;
static int g_shm_fd = -1;

static volatile sig_atomic_t g_running = 1;        // sinyal güvenli
static volatile sig_atomic_t g_sigchld_flag = 0;   // SIGCHLD geldi mi?

static pthread_t g_monitor_thread;
static pthread_t g_ipc_thread;
static pid_t g_instance_pid;

/* ---------- Log ---------- */
static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* ---------- Semaphore lock/unlock ---------- */
static void lock_shared(void) {
    if (sem_wait(g_sem) == -1) {
        perror("sem_wait");
        exit(EXIT_FAILURE);
    }
}

static void unlock_shared(void) {
    if (sem_post(g_sem) == -1) {
        perror("sem_post");
        exit(EXIT_FAILURE);
    }
}

/* ---------- Shared memory yardımcıları ---------- */
static void compact_processes(void) {
    int j = 0;
    for (int i = 0; i < g_shared->process_count; i++) {
        if (g_shared->processes[i].is_active) {
            if (i != j) g_shared->processes[j] = g_shared->processes[i];
            j++;
        }
    }
    g_shared->process_count = j;
}

static void add_process_info(pid_t pid, ProcessMode mode, const char *command) {
    lock_shared();

    if (g_shared->process_count >= MAX_PROCESSES) {
        unlock_shared();
        log_msg("[ERROR] Process tablosu dolu, yeni process eklenemiyor.");
        return;
    }

    ProcessInfo *info = &g_shared->processes[g_shared->process_count++];
    info->pid = pid;
    info->owner_pid = g_instance_pid;

    strncpy(info->command, command, MAX_CMD_LEN - 1);
    info->command[MAX_CMD_LEN - 1] = '\0';

    // sonda '\n' varsa temizle
    size_t len = strlen(info->command);
    if (len > 0 && info->command[len - 1] == '\n') {
        info->command[len - 1] = '\0';
    }

    info->mode = mode;
    info->status = PROCESS_STATUS_RUNNING;
    info->start_time = time(NULL);
    info->is_active = 1;

    unlock_shared();
}

/* ---------- Message queue yardımcı ---------- */
static void send_message(MessageCommand cmd, pid_t target_pid) {
    if (g_mq == (mqd_t)-1) return;

    Message msg;
    msg.msg_type = 1;
    msg.command = cmd;
    msg.sender_pid = g_instance_pid;
    msg.target_pid = target_pid;

    if (mq_send(g_mq, (const char *)&msg, MQ_MSGSIZE, 0) == -1) {
        perror("mq_send");
    }
}

/* ---------- Komut parse ---------- */
static void parse_command(const char *input, char **argv, int max_args) {
    static char buffer[MAX_CMD_LEN];

    strncpy(buffer, input, MAX_CMD_LEN);
    buffer[MAX_CMD_LEN - 1] = '\0';

    int idx = 0;
    char *token = strtok(buffer, " \t\r\n");
    while (token && idx < max_args - 1) {
        argv[idx++] = token;
        token = strtok(NULL, " \t\r\n");
    }
    argv[idx] = NULL;
}

/* ---------- Process başlatma ---------- */
static void start_process(void) {
    char cmd_buf[MAX_CMD_LEN];
    int mode_int;

    printf("Çalıştırılacak komutu girin: ");
    fflush(stdout);

    if (!fgets(cmd_buf, sizeof(cmd_buf), stdin)) return;
    if (cmd_buf[0] == '\n') return;

    printf("Mod seçin (0: Attached, 1: Detached): ");
    fflush(stdout);

    if (scanf("%d%*c", &mode_int) != 1) return;

    ProcessMode mode = (mode_int == 1) ? PROCESS_MODE_DETACHED : PROCESS_MODE_ATTACHED;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        char *argv[32];
        parse_command(cmd_buf, argv, 32);
        if (!argv[0]) _exit(EXIT_FAILURE);

        if (mode == PROCESS_MODE_DETACHED) {
            if (setsid() == -1) {
                perror("setsid");
                _exit(EXIT_FAILURE);
            }
        }

        execvp(argv[0], argv);
        perror("execvp");
        _exit(EXIT_FAILURE);
    }

    add_process_info(pid, mode, cmd_buf);
    send_message(MSG_PROCESS_STARTED, pid);
    log_msg("[SUCCESS] Process başlatıldı: PID %d", pid);
}

/* ---------- Process listeleme ---------- */
static void list_processes(void) {
    lock_shared();

    printf("\nÇALIŞAN PROGRAMLAR\n");
    printf("%-8s %-20s %-10s %-10s %-8s\n", "PID", "Command", "Mode", "Owner", "Süre");

    time_t now = time(NULL);

    for (int i = 0; i < g_shared->process_count; i++) {
        ProcessInfo *p = &g_shared->processes[i];
        if (!p->is_active) continue;

        long sure = (long)(now - p->start_time);
        const char *mode_str = (p->mode == PROCESS_MODE_DETACHED) ? "Detached" : "Attached";

        printf("%-8d %-20s %-10s %-10d %-8ld\n",
               p->pid, p->command, mode_str, p->owner_pid, sure);
    }

    printf("Toplam: %d process\n\n", g_shared->process_count);
    unlock_shared();
}

/* ---------- Process sonlandırma ---------- */
static void terminate_process(void) {
    pid_t pid;

    printf("Sonlandırılacak process PID: ");
    fflush(stdout);

    if (scanf("%d%*c", &pid) != 1) return;

    if (kill(pid, SIGTERM) == -1) {
        perror("kill");
    } else {
        log_msg("[INFO] Process %d'e SIGTERM sinyali gönderildi", pid);

        
        send_message(MSG_PROCESS_KILLED, pid);
    }
}

/* ---------- Monitor thread ---------- */
static void *monitor_thread_fn(void *arg) {
    (void)arg;

    while (g_running) {
        sleep(MONITOR_INTERVAL_SEC);

       
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
        }

        lock_shared();

        for (int i = 0; i < g_shared->process_count; i++) {
            ProcessInfo *p = &g_shared->processes[i];
            if (!p->is_active) continue;
            if (p->owner_pid != g_instance_pid) continue;

            int status;
            pid_t res = waitpid(p->pid, &status, WNOHANG);
            if (res > 0) {
                p->is_active = 0;
                p->status = PROCESS_STATUS_TERMINATED;
                log_msg("[MONITOR] Process %d sonlandı", p->pid);
                send_message(MSG_PROCESS_TERMINATED, p->pid);
            }
        }

        compact_processes();
        unlock_shared();
    }

    return NULL;
}

/* ---------- IPC listener thread ---------- */
static void *ipc_listener_thread_fn(void *arg) {
    (void)arg;

    while (g_running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;

        Message msg;
        ssize_t n = mq_timedreceive(g_mq, (char *)&msg, MQ_MSGSIZE, NULL, &ts);

        if (n == -1) {
            if (errno != ETIMEDOUT && g_running) {
                perror("mq_timedreceive");
            }
            continue;
        }

        if (msg.sender_pid == g_instance_pid) continue;

        if (msg.command == MSG_PROCESS_STARTED) {
            log_msg("[IPC] Yeni process başlatıldı: PID %d", msg.target_pid);
        } else if (msg.command == MSG_PROCESS_TERMINATED) {
            log_msg("[IPC] Process sonlandırıldı: PID %d", msg.target_pid);
        } else if (msg.command == MSG_PROCESS_KILLED) {
            log_msg("[IPC] Process kill edildi: PID %d", msg.target_pid);
        }
    }

    return NULL;
}

/* ---------- IPC init ---------- */
static void init_shared_memory(void) {
    g_shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_shm_fd == -1) {
        perror("shm_open");
        exit(EXIT_FAILURE);
    }

    struct stat st;
    if (fstat(g_shm_fd, &st) == -1) {
        perror("fstat");
        exit(EXIT_FAILURE);
    }

    bool first_time = (st.st_size == 0);

    if (first_time) {
        if (ftruncate(g_shm_fd, sizeof(SharedData)) == -1) {
            perror("ftruncate");
            exit(EXIT_FAILURE);
        }
    }

    g_shared = mmap(NULL, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_shared == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    if (first_time) {
        memset(g_shared, 0, sizeof(SharedData));
    }
}

static void init_semaphore(void) {
    g_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (g_sem == SEM_FAILED) {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }
}

static void init_message_queue(void) {
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = MQ_MAXMSG;
    attr.mq_msgsize = MQ_MSGSIZE;

    g_mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0666, &attr);
    if (g_mq == (mqd_t)-1) {
        perror("mq_open");
        exit(EXIT_FAILURE);
    }
}

/* ---------- IPC cleanup ---------- */
static void cleanup_ipc(void) {
    if (g_shared && g_shared != MAP_FAILED) munmap(g_shared, sizeof(SharedData));
    if (g_shm_fd != -1) close(g_shm_fd);
    if (g_sem && g_sem != SEM_FAILED) sem_close(g_sem);
    if (g_mq != (mqd_t)-1) mq_close(g_mq);
}

/* ---------- Çıkışta attached process'leri öldür ---------- */
static void terminate_attached_processes(void) {
    lock_shared();

    for (int i = 0; i < g_shared->process_count; i++) {
        ProcessInfo *p = &g_shared->processes[i];
        if (!p->is_active) continue;
        if (p->owner_pid != g_instance_pid) continue;

        if (p->mode == PROCESS_MODE_ATTACHED) {
            kill(p->pid, SIGTERM);
            p->is_active = 0;
            p->status = PROCESS_STATUS_TERMINATED;
            send_message(MSG_PROCESS_KILLED, p->pid);
        }
    }

    compact_processes();
    unlock_shared();
}

/* ---------- Signal handler ---------- */
static void handle_signal(int s) {
    if (s == SIGCHLD) {
        g_sigchld_flag = 1;
        return;
    }
    g_running = 0;
}

/* ---------- Menü ---------- */
static void print_menu(void) {
    printf("ProcX v1.0\n");
    printf("1. Yeni Program Çalıştır\n");
    printf("2. Çalışan Programları Listele\n");
    printf("3. Program Sonlandır\n");
    printf("0. Çıkış\n");
    printf("Seçiminiz: ");
    fflush(stdout);
}

/* ---------- main ---------- */
int main(void) {
    g_instance_pid = getpid();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    // SIGINT, SIGTERM, SIGCHLD yönetimi (proje şartı)
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    init_shared_memory();
    init_semaphore();
    init_message_queue();

    if (pthread_create(&g_monitor_thread, NULL, monitor_thread_fn, NULL) != 0) {
        perror("pthread_create(monitor)");
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&g_ipc_thread, NULL, ipc_listener_thread_fn, NULL) != 0) {
        perror("pthread_create(ipc_listener)");
        exit(EXIT_FAILURE);
    }

    int choice;
    while (g_running) {
        print_menu();

        if (scanf("%d%*c", &choice) != 1) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            continue;
        }

        switch (choice) {
            case 1: start_process(); break;
            case 2: list_processes(); break;
            case 3: terminate_process(); break;
            case 0: g_running = 0; break;
            default: printf("[WARN] Geçersiz seçim.\n"); break;
        }
    }

    // Thread'ler düzgün kapansın
    pthread_join(g_monitor_thread, NULL);
    pthread_join(g_ipc_thread, NULL);

    // Attached’leri öldür, detached’leri bırak
    terminate_attached_processes();

    // IPC kapat
    cleanup_ipc();

    printf("Çıkılıyor...\n");
    return 0;
}
