#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "vars.h"
#include "zlog.h"
#include "utils.h"

int server = -1, client = -1, running = 0;
pid_t forkpid = 1;
char buf[BUF_SIZE];

/**
 * handle Ctrl-C 
 * 
 * @param {int} socket fd
 *
 * the parent and all its children will recieve kill signal and execute this
 */
void ouch(int n) {
    running = 0;
    puts("");
    if (forkpid > 0) {      // ftpd
        if (server >= 0) {
            int st = close(server);
            info(0, "shutdown ftp ... %d", st);
        }
    } else {                // session
        if (client >= 0) {
            int st = close(client);
            info(1, "shutdown ftp session... %d", st);
        }
    }
    exit(0);
}

enum FTP_CMD parse_cmd(char *buf, int len) {
    int i,j;
    for (i=0; i<FTP_CMD_COUNT; i++) {
        for (j=0; FTP_CMD_LIST[i].name[j] != '\0' && j < len; j++) {
            if (FTP_CMD_LIST[i].name[j] != buf[j] && FTP_CMD_LIST[i].name[j] != buf[j]-32) break;
        }
        if (FTP_CMD_LIST[i].name[j] == '\0')
            return FTP_CMD_LIST[i].cmd;
    }
    return INVALID;
}

/**
 * handle a newly accepted ftp session
 *
 */
void handle_session(int client) {
    send_str(client, FTP_RDY);
    int i, n, retry;
    char cwd[BUF_SIZE] = {0}, cmdbuf[BUF_SIZE] = {0};
    enum DATA_TYPE datatype = TYPE_IMAGE;
    srand(time(0));
    uint32_t pasv_port;
    enum TRSF_TYPE trsf_type = TRSF_PORT;
    int pasv_server = -1;
    struct sockaddr_in svr_addr;
    int svr_addr_len = sizeof(svr_addr);
    getsockname(client, (struct sockaddr*)&svr_addr, &svr_addr_len);
    uint32_t svr_host_addr = ntohl(svr_addr.sin_addr.s_addr);
    uint32_t port_address = 0;
    uint16_t port_port = 0;
    int data_client = -1;
    struct sockaddr_in data_client_addr;
    int data_client_len = sizeof(data_client_addr);

    while ((n=recv(client, buf, BUF_SIZE, MSG_PEEK)) > 0) {
        if (!running) break;
        buf[n] = '\0';
        //info(1, "recved %d bytes: %s", n, buf);
        for (i=0; i<n; i++) {
            if (buf[i] == '\n') break;
        }
        if (buf[i] != '\n') {
            err(1, "no line break found");
            break;
        }
        n = recv(client, buf, i+1, 0);
        buf[n] = '\0';
        enum FTP_CMD cmd = parse_cmd(buf, n);
        if (cmd < 0) {
            buf[n-2] = 0;
            err(1, "unknown cmd: %s", buf);
            continue;
        }
        info(1, "cmd: %s, %d", FTP_CMD_LIST[cmd].name, cmd);
        switch(cmd) {
            case NOOP:
                send_str(client, FTP_OK);
                break;
            case QUIT:
                send_str(client, FTP_QUIT);
                running = 0;
                break;
            case HELP:
                send_str(client, FTP_HELP);
                break;
            case USER:
                send_str(client, FTP_NAMEOK);
                break;
            case PASS:
                send_str(client, FTP_LOGIN);
                break;
            case PWD:
                getcwd(cwd, sizeof(cwd));
                send_str(client, FTP_PWD, cwd);
                break;
            case SYST:
                send_str(client, FTP_SYST);
                break;
            case TYPE:
                if (buf[5] == 'A') {
                    datatype = TYPE_ASCII;
                    send_str(client, FTP_CTYPE, buf[5]);
                } else if (buf[5] == 'I') {
                    datatype = TYPE_IMAGE;
                    send_str(client, FTP_CTYPE, buf[5]);
                } else {
                    send_str(client, FTP_ERR_DATATYPE, datatype == TYPE_ASCII ? 'A' : 'I');
                }
                break;
            case PASV:
                retry = 100;
                while (retry--) { // in case of create server error, port used
                    pasv_port = (rand() % 64512 + 1024);
                    trsf_type = TRSF_PASV;
                    pasv_server = new_server(INADDR_ANY, pasv_port, 1);
                    if (pasv_server >= 0) break;
                }
                if (pasv_server < 0) {
                    err(1, "can not create pasv port for passive mode");
                    // TODO: send err msg here
                } else {
                    uint32_t t = svr_addr.sin_addr.s_addr;
                    send_str(client, FTP_PASV, t&0xff, (t>>8)&0xff, (t>>16)&0xff, (t>>24)&0xff, pasv_port>>8, pasv_port & 0xff);
                }
                break;
            case PORT:
                trsf_type = TRSF_PORT;
                int _st = parse_addr_port(buf, &port_address, &port_port);
                if (!_st) {
                    err(1, "port cmd error parsing addr and port");
                    send_str(client, FTP_ERR_PORT);
                } else {
                    info(1, "address is %s, port is %ld", n2a(port_address), port_port);
                    send_str(client, FTP_PORT);
                }
                break;
            case LIST:
                if (trsf_type == TRSF_PASV) {
                    if (pasv_port > 1024 && pasv_port <= 65535 && pasv_server >= 0) {
                        send_str(client, FTP_ASCII);
                        data_client = accept(pasv_server, (struct sockaddr *)&data_client_addr, &data_client_len);
                        if (data_client < 0) {
                            err(1, "accept data client socket error");
                        }
                    } else {
                        err(1, "no pasv server created");
                        break;
                    }
                } else if (trsf_type == TRSF_PORT) {
                    if (port_address == 0 || port_port == 0) {
                        err(1, "LIST cmd in PORT mode, address and port not set before");
                        break;
                    }
                    send_str(client, FTP_ASCII);
                    info(1, "LIST cmd in PORT mode, try connecting %s %lu", n2a(port_address), port_port);
                    data_client = new_client(port_address, port_port);
                    info(1, "R");
                    if (data_client < 0) {
                        err(1, "port mode connect client data sock error");
                        break;
                    }
                } else {
                    err(1, "transfer type no specified");
                }
                if (data_client >= 0) {
                    getcwd(cwd, sizeof(cwd));
                    sprintf(cmdbuf, "ls -l %s", cwd);
                    FILE *p = popen(cmdbuf, "r");
                    send_file(data_client, p);
                    pclose(p);
                } else {
                    err(1, "no data client created");
                }
                break;
        }
        if (!running) break;
    }
    info(1, "exit session");
    int st = close(client);
    info(1, "closed , status %d", st);
}

int main(int argc, char *argv[]){
    int port = LISTEN_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    signal(SIGCHLD, SIG_IGN);   // ignore child termination signal
    signal(SIGINT, ouch);       // catch Ctrl-C 
    signal(SIGTERM, ouch);

    server = new_server(LISTEN_ADDR, port, MAX_CONNECTIONS);
    if (server < 0) {
        err(0, "can not create server, return code is %d, socket already in use", server);
        exit(1);
    }

    running = 1;

    struct sockaddr_in client_addr;
    while (running) {
        uint32_t l = sizeof(client_addr);
        client = accept(server, (struct sockaddr *)&client_addr, &l);

        if (!running) break;
        if (client < 0) {
            err(0, "accept client error: %d", client);
            exit(2);
        }
        info(0, "client connected: %s", inet_ntoa(client_addr.sin_addr));

        forkpid = fork();
        if (forkpid == -1) {
            err(0, "fork server error");
        } else if (forkpid == 0) {      // child
            server = -1;        // avoid killing server on Ctrl-C
            info(0, "new ftp session");
            handle_session(client);
            exit(0);
        } else if (forkpid > 0) {       // myself
            client = -1;
        }
    }
    
    info(0, "exit ftpd");
    return 0;
}

