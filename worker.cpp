#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <cstdlib>
#include <signal.h>

using namespace std;

#define PERMS 0644

// Shared memory structure for system clock
struct Clock {
    int seconds;
    int nanoseconds;
};

// Message buffer structure for IPC
struct msgbuffer {
    long mtype;      // Message type (PID of recipient)
    int intData;     // Data: 1 = running, 0 = terminating
};

int main(int argc, char** argv) {
    // Handle termination signal gracefully
    signal(SIGTERM, SIG_DFL);
    
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <seconds> <nanoseconds>" << endl;
        return EXIT_FAILURE;
    }
    
    // Get duration from command line
    int durationSeconds = atoi(argv[1]);
    int durationNano = atoi(argv[2]);
    
    // Attach to shared memory
    key_t shmkey = ftok("oss.cpp", 1);
    int shmid = shmget(shmkey, sizeof(Clock), PERMS);
    if (shmid == -1) {
        perror("Worker: Failed to access shared memory");
        return EXIT_FAILURE;
    }
    
    Clock* systemClock = (Clock*)shmat(shmid, NULL, 0);
    if (systemClock == (void*)-1) {
        perror("Worker: Failed to attach to shared memory");
        return EXIT_FAILURE;
    }
    
    // Access message queue
    key_t msgkey = ftok("worker.cpp", 1);
    int msqid = msgget(msgkey, PERMS);
    if (msqid == -1) {
        perror("Worker: Failed to access message queue");
        return EXIT_FAILURE;
    }
    
    // Calculate termination time based on current clock + duration
    int termSeconds = systemClock->seconds + durationSeconds;
    int termNano = systemClock->nanoseconds + durationNano;
    if (termNano >= 1000000000) {
        termSeconds += 1;
        termNano -= 1000000000;
    }
    
    // Print startup information
    cout << "WORKER PID:" << getpid() << " PPID:" << getppid() 
         << " SysClockS: " << systemClock->seconds 
         << " SysclockNano: " << systemClock->nanoseconds << endl;
    cout << "TermTimeS: " << termSeconds << " TermTimeNano: " << termNano << endl;
    cout << "--Just Starting" << endl;
    
    int messagesReceived = 0;
    bool shouldTerminate = false;
    
    // Main worker loop
    while (!shouldTerminate) {
        // Wait for message from oss
        msgbuffer msg;
        if (msgrcv(msqid, &msg, sizeof(msgbuffer) - sizeof(long), getpid(), 0) == -1) {
            perror("Worker: Failed to receive message");
            break;
        }
        
        messagesReceived++;
        
        // Check if termination time has been reached
        if (systemClock->seconds > termSeconds || 
            (systemClock->seconds == termSeconds && systemClock->nanoseconds >= termNano)) {
            shouldTerminate = true;
        }
        
        // Print status
        cout << "WORKER PID:" << getpid() << " PPID:" << getppid() 
             << " SysClockS: " << systemClock->seconds 
             << " SysclockNano: " << systemClock->nanoseconds << endl;
        cout << "TermTimeS: " << termSeconds << " TermTimeNano: " << termNano << endl;
        
        if (shouldTerminate) {
            cout << "--Terminating after sending message back to oss after " 
                 << messagesReceived << " received messages." << endl;
        } else {
            cout << "--" << messagesReceived << " messages received from oss" << endl;
        }
        
        // Send response back to parent
        msgbuffer response;
        response.mtype = getppid();
        response.intData = shouldTerminate ? 0 : 1;
        
        if (msgsnd(msqid, &response, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
            perror("Worker: Failed to send message");
            break;
        }
    }
    
    // Detach from shared memory
    shmdt(systemClock);
    return EXIT_SUCCESS;
}
