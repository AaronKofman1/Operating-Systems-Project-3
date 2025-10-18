#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <signal.h>
#include <ctime>

using namespace std;

#define PERMS 0644
#define MAX_PROCESSES 20

// Shared memory structure for system clock
struct Clock {
    int seconds;
    int nanoseconds;
};

// Process Control Block structure
struct PCB {
    int occupied;         // 1 if in use, 0 if free
    pid_t pid;           // Process ID
    int startSeconds;    // Start time in seconds
    int startNano;       // Start time in nanoseconds
    int messagesSent;    // Total messages sent to this worker
};

// Message buffer structure for IPC
struct msgbuffer {
    long mtype;          // Message type (PID of recipient)
    int intData;         // Data: 1 = running, 0 = terminating
};

// Global variables for cleanup
int shmid = -1;
int msqid = -1;
Clock* systemClock = NULL;
pid_t childPids[MAX_PROCESSES];
int childPidCount = 0;

// Cleanup function to remove IPC resources
void cleanup() {
    if (systemClock != NULL) {
        shmdt(systemClock);
    }
    if (shmid != -1) {
        shmctl(shmid, IPC_RMID, NULL);
    }
    if (msqid != -1) {
        msgctl(msqid, IPC_RMID, NULL);
    }
}

// Signal handler for SIGINT and SIGALRM
void signalHandler(int sig) {
    cout << "\nOSS: Caught signal " << sig << ", cleaning up..." << endl;
    
    // Terminate all child processes
    for (int i = 0; i < childPidCount; i++) {
        if (childPids[i] > 0) {
            kill(childPids[i], SIGTERM);
        }
    }
    
    // Wait for children to finish
    for (int i = 0; i < childPidCount; i++) {
        if (childPids[i] > 0) {
            waitpid(childPids[i], NULL, WNOHANG);
        }
    }
    
    cleanup();
    exit(0);
}

// Display help message
void printHelp(const char* programName) {
    cout << "Usage: " << programName << " [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i intervalInMsToLaunchChildren] [-f logfile]" << endl;
    cout << "  -h : Show this help message" << endl;
    cout << "  -n : Total number of child processes (default: 5)" << endl;
    cout << "  -s : Number of children allowed to run simultaneously (default: 3)" << endl;
    cout << "  -t : Time limit for children in seconds (default: 5)" << endl;
    cout << "  -i : Interval in seconds to launch children (default: 0.5)" << endl;
    cout << "  -f : Log file name (default: log.txt)" << endl;
}

// Increment system clock by 250ms / numChildren
void incrementClock(int numChildren) {
    int incrementNano = (numChildren > 0) ? (250000000 / numChildren) : 250000000;
    systemClock->nanoseconds += incrementNano;
    if (systemClock->nanoseconds >= 1000000000) {
        systemClock->seconds++;
        systemClock->nanoseconds -= 1000000000;
    }
}

// Print process table to console and log file
void printProcessTable(PCB* processTable, ofstream& logFile) {
    cout << "OSS PID:" << getpid() << " SysClockS: " << systemClock->seconds 
         << " SysclockNano: " << systemClock->nanoseconds << endl;
    cout << "Process Table:" << endl;
    cout << "Entry\tOccupied\tPID\tStartS\tStartN\t\tMessagesSent" << endl;
    
    logFile << "OSS PID:" << getpid() << " SysClockS: " << systemClock->seconds 
            << " SysclockNano: " << systemClock->nanoseconds << endl;
    logFile << "Process Table:" << endl;
    logFile << "Entry\tOccupied\tPID\tStartS\tStartN\t\tMessagesSent" << endl;
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        cout << i << "\t" << processTable[i].occupied << "\t\t" 
             << processTable[i].pid << "\t" << processTable[i].startSeconds << "\t"
             << processTable[i].startNano << "\t" << processTable[i].messagesSent << endl;
        
        logFile << i << "\t" << processTable[i].occupied << "\t\t" 
                << processTable[i].pid << "\t" << processTable[i].startSeconds << "\t"
                << processTable[i].startNano << "\t" << processTable[i].messagesSent << endl;
    }
    cout << endl;
    logFile << endl;
}

int main(int argc, char** argv) {
    srand(time(NULL));
    
    // Default command line parameter values
    int totalChildren = 5;
    int simultaneous = 3;
    double timeLimit = 5.0;
    double launchInterval = 0.5;
    string logFileName = "log.txt";
    
    // Parse command line arguments
    int option;
    while ((option = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (option) {
            case 'h':
                printHelp(argv[0]);
                return EXIT_SUCCESS;
            case 'n':
                totalChildren = atoi(optarg);
                break;
            case 's':
                simultaneous = atoi(optarg);
                break;
            case 't':
                timeLimit = atof(optarg);
                break;
            case 'i':
                launchInterval = atof(optarg);
                break;
            case 'f':
                logFileName = optarg;
                break;
            default:
                return EXIT_FAILURE;
        }
    }
    
    // Set up signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGALRM, signalHandler);
    alarm(60);  // 60 second timeout
    
    // Create shared memory for clock
    key_t shmkey = ftok("oss.cpp", 1);
    shmid = shmget(shmkey, sizeof(Clock), PERMS | IPC_CREAT);
    if (shmid == -1) {
        perror("Failed to create shared memory");
        return EXIT_FAILURE;
    }
    
    systemClock = (Clock*)shmat(shmid, NULL, 0);
    if (systemClock == (void*)-1) {
        perror("Failed to attach shared memory");
        return EXIT_FAILURE;
    }
    
    // Initialize clock to zero
    systemClock->seconds = 0;
    systemClock->nanoseconds = 0;
    
    // Create message queue
    key_t msgkey = ftok("worker.cpp", 1);
    msqid = msgget(msgkey, PERMS | IPC_CREAT);
    if (msqid == -1) {
        perror("Failed to create message queue");
        cleanup();
        return EXIT_FAILURE;
    }
    
    // Open log file
    ofstream logFile(logFileName);
    if (!logFile.is_open()) {
        cerr << "Failed to open log file" << endl;
        cleanup();
        return EXIT_FAILURE;
    }
    
    // Initialize process table
    PCB processTable[MAX_PROCESSES];
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processTable[i].occupied = 0;
        processTable[i].pid = 0;
        processTable[i].startSeconds = 0;
        processTable[i].startNano = 0;
        processTable[i].messagesSent = 0;
    }
    
    // Initialize child PID tracking
    for (int i = 0; i < MAX_PROCESSES; i++) {
        childPids[i] = 0;
    }
    
    // Tracking variables
    int childrenLaunched = 0;
    int currentlyRunning = 0;
    int nextToSend = 0;
    int totalMessagesSent = 0;
    
    // Launch timing variables
    int launchIntervalNano = (int)(launchInterval * 1000000000);
    int nextLaunchSeconds = 0;
    int nextLaunchNano = 0;
    
    // Process table printing timing
    int lastTablePrintSeconds = 0;
    int lastTablePrintNano = 0;
    
    // Main simulation loop
    while (childrenLaunched < totalChildren || currentlyRunning > 0) {
        // Increment the system clock
        incrementClock(currentlyRunning > 0 ? currentlyRunning : 1);
        
        // Print process table every half second
        long long timeSinceLastPrint = ((long long)(systemClock->seconds - lastTablePrintSeconds) * 1000000000) 
                                + (systemClock->nanoseconds - lastTablePrintNano);
        if (timeSinceLastPrint >= 500000000) {
            printProcessTable(processTable, logFile);
            lastTablePrintSeconds = systemClock->seconds;
            lastTablePrintNano = systemClock->nanoseconds;
        }
        
        // Launch new worker if conditions are met
        if (childrenLaunched < totalChildren && currentlyRunning < simultaneous) {
            if (systemClock->seconds > nextLaunchSeconds || 
                (systemClock->seconds == nextLaunchSeconds && systemClock->nanoseconds >= nextLaunchNano)) {
                
                // Find free slot in process table
                int tableIndex = -1;
                for (int i = 0; i < MAX_PROCESSES; i++) {
                    if (processTable[i].occupied == 0) {
                        tableIndex = i;
                        break;
                    }
                }
                
                if (tableIndex != -1) {
                    // Generate random duration for worker
                    int randSeconds = (rand() % (int)timeLimit) + 1;
                    int randNano = rand() % 1000000000;
                    
                    pid_t childPid = fork();
                    if (childPid == 0) {
                        // Child process - exec to worker
                        char secStr[20], nanoStr[20];
                        sprintf(secStr, "%d", randSeconds);
                        sprintf(nanoStr, "%d", randNano);
                        execl("./worker", "./worker", secStr, nanoStr, (char*)NULL);
                        perror("Exec failed");
                        exit(1);
                    } else if (childPid > 0) {
                        // Parent process - update process table
                        processTable[tableIndex].occupied = 1;
                        processTable[tableIndex].pid = childPid;
                        processTable[tableIndex].startSeconds = systemClock->seconds;
                        processTable[tableIndex].startNano = systemClock->nanoseconds;
                        processTable[tableIndex].messagesSent = 0;
                        
                        // Track child PID for signal handling
                        childPids[childPidCount++] = childPid;
                        
                        childrenLaunched++;
                        currentlyRunning++;
                        
                        cout << "OSS: Launched worker " << tableIndex << " PID " << childPid << endl;
                        logFile << "OSS: Launched worker " << tableIndex << " PID " << childPid << endl;
                        
                        printProcessTable(processTable, logFile);
                        
                        // Calculate next launch time
                        nextLaunchNano = systemClock->nanoseconds + launchIntervalNano;
                        nextLaunchSeconds = systemClock->seconds;
                        if (nextLaunchNano >= 1000000000) {
                            nextLaunchSeconds++;
                            nextLaunchNano -= 1000000000;
                        }
                    }
                }
            }
        }
        
        // Send message to next active worker (round-robin)
        if (currentlyRunning > 0) {
            // Find next occupied slot
            int attempts = 0;
            while (attempts < MAX_PROCESSES) {
                if (processTable[nextToSend].occupied == 1) {
                    break;
                }
                nextToSend = (nextToSend + 1) % MAX_PROCESSES;
                attempts++;
            }
            
            if (processTable[nextToSend].occupied == 1) {
                pid_t targetPid = processTable[nextToSend].pid;
                
                // Prepare and send message
                msgbuffer msg;
                msg.mtype = targetPid;
                msg.intData = 1;
                
                cout << "OSS: Sending message to worker " << nextToSend << " PID " << targetPid 
                     << " at time " << systemClock->seconds << ":" << systemClock->nanoseconds << endl;
                logFile << "OSS: Sending message to worker " << nextToSend << " PID " << targetPid 
                        << " at time " << systemClock->seconds << ":" << systemClock->nanoseconds << endl;
                
                if (msgsnd(msqid, &msg, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
                    perror("Failed to send message");
                }
                processTable[nextToSend].messagesSent++;
                totalMessagesSent++;
                
                // Receive response from worker
                msgbuffer rcvbuf;
                if (msgrcv(msqid, &rcvbuf, sizeof(msgbuffer) - sizeof(long), getpid(), 0) == -1) {
                    perror("Failed to receive message");
                }
                
                cout << "OSS: Received message from worker " << nextToSend << " PID " << targetPid 
                     << " at time " << systemClock->seconds << ":" << systemClock->nanoseconds << endl;
                logFile << "OSS: Received message from worker " << nextToSend << " PID " << targetPid 
                        << " at time " << systemClock->seconds << ":" << systemClock->nanoseconds << endl;
                
                // Check if worker is terminating
                if (rcvbuf.intData == 0) {
                    cout << "OSS: Worker " << nextToSend << " PID " << targetPid << " is planning to terminate" << endl;
                    logFile << "OSS: Worker " << nextToSend << " PID " << targetPid << " is planning to terminate" << endl;
                    
                    wait(NULL);
                    processTable[nextToSend].occupied = 0;
                    currentlyRunning--;
                }
                
                nextToSend = (nextToSend + 1) % MAX_PROCESSES;
            }
        }
    }
    
    // Print summary
    cout << "\n=== OSS Summary ===" << endl;
    cout << "Total processes launched: " << childrenLaunched << endl;
    cout << "Total messages sent: " << totalMessagesSent << endl;
    
    logFile << "\n=== OSS Summary ===" << endl;
    logFile << "Total processes launched: " << childrenLaunched << endl;
    logFile << "Total messages sent: " << totalMessagesSent << endl;
    
    logFile.close();
    cleanup();
    return EXIT_SUCCESS;
}
