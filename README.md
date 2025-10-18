How to compile the project:
Type 'make'

Example of how to run the project:
./oss -n 5 -s 3 -t 7 -i 0.5 -f log.txt

Description:
This project implements a parent process (oss) that manages multiple worker processes
using message queues and shared memory. The oss process maintains a simulated system
clock and coordinates worker processes through message passing.
