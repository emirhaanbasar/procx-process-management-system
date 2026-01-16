# procx-process-management-system
# ProcX – Process Management System

ProcX is a UNIX-like process management system developed as a term project for the
Operating Systems course.

## Project Goal
The goal of this project is to gain hands-on experience with:
- Process management
- Multithreading
- Inter-Process Communication (IPC)

## Features
- Start new processes (fork + exec)
- List running processes
- Terminate processes by PID
- Shared process table across multiple terminal instances
- Attached and Detached process modes

## Architecture
The application uses three main threads:
- Main Thread: Handles user input and process operations
- Monitor Thread: Detects terminated processes using waitpid(WNOHANG)
- IPC Listener Thread: Synchronizes multiple instances via message queues

## IPC Mechanisms Used
- Shared Memory: Stores a global process table
- Semaphore: Protects shared memory access
- POSIX Message Queue: Sends process start/termination events between instances

## Build & Run
```bash
make
./procx
