```bash

##################### How to compile #####################
gcc -O2 -o server server.c
gcc -O2 -o peer peer.c


####################### How to run #######################

# Initiate server:
./server 5000

# Initiate Peer A
./peer <server_ip> 5000 A B
#./peer <server_ip> <server_port> <a_id> [target_id]

# Initiate Peer B
./peer <server_ip> 5000 B
#./peer <server_ip> <server_port> <b_id>