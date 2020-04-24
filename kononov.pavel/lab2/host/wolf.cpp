#include "wolf.h"

#include <iostream>
#include <limits>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <fcntl.h>
#include "../utils/utils.h"
#include "identifier.h"


ClientInfo *Wolf::client_info;
pthread_t* Wolf::threads;
pthread_attr_t *Wolf::attr;
int Wolf::_clientsNum;
Identifier Wolf::identifier;
int Wolf::curr_num;
std::atomic<int> Wolf::finished;
std::atomic<int> Wolf::step;
pthread_cond_t Wolf::cond;
pthread_mutex_t Wolf::mutx;

void Wolf::Start() {
    int attached_clients = 0;

    while (attached_clients != _clientsNum) {
        attached_clients = 0;
        for (int i = 0; i < _clientsNum; ++i) {
            if (client_info[i].attached)
                attached_clients++;
        }

        if (attached_clients != _clientsNum) {
            std::cout << "Waiting " << _clientsNum - attached_clients << " clients." << std::endl;
            pause();
        }
    }
    std::cout << "Clients attached!" << std::endl;

    for (int i = 0; i < _clientsNum; ++i) {
        pthread_attr_init(&attr[i]);
        pthread_create(&(threads[i]), &attr[i], ThreadRun, &client_info[i]);
    }

    while (true) {
        if (finished == _clientsNum) {
            pthread_mutex_lock(&mutx);
            int res = 0;
            for (int i = 0; i < _clientsNum; ++i) {
                if (client_info[i].count_dead < 2)
                    ++res;
            }

            if (res == 0) {
                std::cout << "All goats are dead during 2 steps, game over." << std::endl;
                Terminate(SIGINT);
            }

            std::cout.flush();
            GetNumber();
            std::cout.flush();
            finished = 0;
            step++;
            pthread_cond_broadcast(&cond);
            pthread_mutex_unlock(&mutx);
        } else {
            sleep(1);
        }
    }
}

bool Wolf::OpenConnection() {
    bool res = true;

    for (int i = 0; i < _clientsNum; ++i) {
        res &= client_info[i].OpenConnection(i);
    }

    if (res)
        std::cout << "Created host pid: " << getpid() << std::endl;
    else
        std::cout << "Open connection error" << std::endl;

    return res;
}

Wolf& Wolf::GetInstance(int n = 1) {
    static Wolf instance(n);
    return instance;
}

void Wolf::Terminate(int signum) {
    std::cout << "Exit" << std::endl;

    for (int i = 0; i < _clientsNum; ++i)
        client_info[i].Dettach();

    for (int i = 0; i < _clientsNum; ++i)
        kill(client_info[i].pid, signum);

    for (int i = 0; i < _clientsNum; i++) {
        client_info[i].Delete();
        pthread_cancel(threads[i]);
    }

    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutx);

    free(threads);
    free(attr);
    delete[] client_info;
    exit(signum);
}

Wolf::Wolf(int n) {
    struct sigaction act;
    act.sa_sigaction = SignalHandler;
    act.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGTERM, &act, nullptr);
    sigaction(SIGINT, &act, nullptr);
    sigaction(SIGUSR1, &act, nullptr);
    sigaction(SIGUSR2, &act, nullptr);

    client_info = new ClientInfo[n];
    threads = (pthread_t *) malloc(n * sizeof(pthread_t));
    attr = (pthread_attr_t *) malloc(n * sizeof(pthread_attr_t));

    pthread_cond_init(&cond, NULL);
    pthread_mutex_init(&mutx, NULL);
    finished = 0;

    _clientsNum = n;
    curr_num = 0;
    identifier = Identifier(n);
    step = 0;
}

Message Wolf::Step(Message &ans, ClientInfo &info) {
    Message msg;
    if ((ans.status == Status::ALIVE && abs(curr_num - ans.number) <= 70) ||
        (ans.status == Status::DEAD && abs(curr_num - ans.number) <= 20)) {
        info.count_dead = 0;
    } else {
        msg.status = Status::DEAD;
        info.count_dead++;
    }
    msg.number = curr_num;
    return msg;
}


void Wolf::GetNumber() {
    std::cout << "Input new host number: " << std::endl;

    int num = -1;
    while (num == -1) {
        std::cin >> num;

        if (!(std::cin.fail())) {
            if (num >= 1 && num <= RAND_LIMIT)
                curr_num = num;
            else {
                std::cout << "Input should be an integer in range [1; " << RAND_LIMIT << "]. Please, try again..."
                          << std::endl;
                num = -1;
            }
        } else {
            num = -1;
            std::cout << "Input should be an integer in range [1; " << RAND_LIMIT << "]. Please, try again..."
                      << std::endl;
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        }
    }
}

void Wolf::SignalHandler(int signum, siginfo_t* info, void* ptr) {
    static Wolf &instance = GetInstance();
    switch (signum) {
        case SIGUSR1: {
            int iClient = instance.identifier.GetId();
            if (iClient == -1) {
                std::cout << "Too many clients" << std::endl;
            } else {

                union sigval value;
                value.sival_int = iClient;
                if (sigqueue(info->si_pid, SIGUSR1, value) == 0) {
                    std::cout << "Attaching client with pid: " << info->si_pid << " and id: " << iClient << std::endl;
                    instance.client_info[iClient].Attach(info->si_pid);
                }
            }
            break;
        }
        case SIGUSR2: {
            for (int i = 0; i < _clientsNum; ++i)
                if (client_info[i].pid == info->si_pid && client_info[i].attached)
                {
                    instance.Terminate(SIGINT);
                    break;
                }
            break;
        }
        default: {
            if (getpid() != info->si_pid) {
                instance.Terminate(signum);
            }
        }
    }
}

void *Wolf::ThreadRun(void *pthrData) {
    ClientInfo *info = (ClientInfo *) pthrData;

    int id = info->id;
    sem_t *semaphore_host = info->semaphore_host;
    sem_t *semaphore_client = info->semaphore_client;
    Conn connection = info->connection;

    int clientStep = 0;
    struct timespec ts;
    Message msg;


    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    finished++;
    sem_post(semaphore_client);
    while (true) {
        if (info->attached) {
            pthread_mutex_lock(&mutx);
            while (clientStep >= step)
                pthread_cond_wait(&cond, &mutx);
            clientStep++;

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += TIMEOUT;
            if (sem_timedwait(semaphore_host, &ts) == -1) {
                std::cout << "Time wait id:" << id << std::endl;
                kill(info->pid, SIGTERM);
                info->Dettach();
                continue;
            }
            if (connection.Read(&msg, sizeof(Message))) {
                std::cout << "--------------------------------" << std::endl;
                std::cout << "Goat[" << id << "] current status: "
                          << ((msg.status == Status::ALIVE) ? "alive" : "dead") << std::endl;
                std::cout << "Goat[" << id << "] number: " << msg.number << std::endl;
                msg = Step(msg, *info);
                std::cout << "Goat[" << id << "] new status: "
                          << ((msg.status == Status::ALIVE) ? "alive" : "dead") << std::endl;

                connection.Write(&msg, sizeof(msg));
            }
            sem_post(semaphore_client);
            ++finished;
            pthread_mutex_unlock(&mutx);
        } else
            pthread_exit(nullptr);
    }
}