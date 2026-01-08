# Multithreaded Bank Server (IPC)

## Description
A robust banking server implementation in C that handles multiple concurrent client connections using POSIX threads. The system ensures data integrity across checking, savings, and transfer operations using mutexes and read-write locks.

For technical details on the synchronization strategy and socket protocol, please see the [Project Report](Project_Report.pdf).

## Key Features
* **Concurrency:** Spawns a new thread for every connected client, allowing simultaneous operations.
* **Synchronization:** Uses **Read-Write Locks** (`pthread_rwlock`) to allow multiple clients to check balances simultaneously while locking the account exclusively during withdrawals or deposits.
* **IPC:** Communicates via **Unix Domain Sockets** (`/tmp/threadbank.sock`) for secure, local client-server interaction.
* **Persistence:** Saves account states to disk to ensure data survives server restarts.

## Technical Details
* **Language:** C (C11 Standard) [cite: 1060]
* **Libraries:** `<pthread.h>`, `<sys/socket.h>`, `<sys/un.h>`
* **Architecture:** Client-Server model using a custom text-based protocol.

## Challenges & Learnings
* **Race Conditions:** Implemented granular locking strategies to prevent "lost updates" when two clients access the same account simultaneously.
* **Socket Management:** Handled `SIGINT` and `SIGTERM` signals to ensure the server closes sockets and saves data cleanly upon shutdown.

## Usage
To build the project:
```bash
make
```
To run the server:
```bash
./client
