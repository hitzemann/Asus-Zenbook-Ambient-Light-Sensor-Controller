/*
   Copyright 2013-2014 Daniele Di Sarli

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <pthread.h>
#include <syslog.h>
#include <errno.h>
#include <err.h>
#include <bsd/libutil.h>
#include "comsock.h"
#include "client.h"

using namespace std;

void logServerExit(int __status, int __pri, const char *fmt);
void startDaemon();
void enableALS(bool enable);
void *IPCHandler(void *arg);
void *clientHandler(void *arg);
int getLidStatus();
int fileExist(const char *filename);

volatile bool active = false;

int g_socket = -1;

const string SOCKET_PATH = "/var/run/als-controller.socket";
char* C_SOCKET_PATH = (char*)SOCKET_PATH.c_str();

pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t start = PTHREAD_COND_INITIALIZER;

/** Signal mask */
static sigset_t g_sigset;


void *sigManager(void *arg) {
  int signum;

  while(1) {
    sigwait(&g_sigset, &signum);

    if(signum == SIGINT || signum == SIGTERM) {
        logServerExit(EXIT_SUCCESS, LOG_INFO, "Terminated.");
    }
  }

  return NULL;
}

void logServerExit(int __status, int __pri, const char *fmt) {
    closeServerChannel(C_SOCKET_PATH, g_socket);
    enableALS(false);
    syslog(__pri, "%s", fmt);
    if(__status != EXIT_SUCCESS)
        syslog(LOG_INFO, "Terminated.");
    closelog();
    exit(__status);
}

int fileExist(const char *filename)
{
    struct stat buffer;
    return (stat (filename, &buffer) == 0);
}

void writeAttribute(string path, string data) {
    int fd = open(path.c_str(), O_WRONLY);
    if(fd == -1) {
        string msg = "Error opening " + path;
        logServerExit(EXIT_FAILURE, LOG_CRIT, msg.c_str());
    }
    if(write(fd, data.c_str(), data.length() + 1) == -1) {
        string msg = "Error writing to " + path;
        logServerExit(EXIT_FAILURE, LOG_CRIT, msg.c_str());
    }

    close(fd);
}

void enableALS(bool enable) {
    if(enable) {
        writeAttribute("/sys/bus/acpi/devices/ACPI0008:00/enable", "1");
    } else {
        writeAttribute("/sys/bus/acpi/devices/ACPI0008:00/enable", "0");
    }

    if (enable)
        syslog(LOG_INFO, "ALS enabled");
    else
        syslog(LOG_INFO, "ALS disabled");
}

string getScreenBacklightDevicePath() {
    string path0 = "/sys/class/backlight/acpi_video0/";
    string path1 = "/sys/class/backlight/intel_backlight/";
    if (fileExist(path0.c_str())) {
        return path0;
    } else {
        return path1;
    }
}

int getScreenBacklightMax()
{
    string path = getScreenBacklightDevicePath() + "max_brightness";

    int fd = open(path.c_str(), O_RDONLY);
    if(fd == -1) {
        syslog(LOG_ERR, "Error opening %s", path.c_str());
        return 0;
    }
    
    char str[100];
    int count = read(fd, str, sizeof(str));
    if (count == -1) return 0;
    str[count] = '\0';
    close(fd);
    int value = atoi(str);
    return value;
}

int getScreenBacklightCur()
{
    string path = getScreenBacklightDevicePath() + "brightness";

    int fd = open(path.c_str(), O_RDONLY);
    if(fd == -1) {
        syslog(LOG_ERR, "Error opening %s", path.c_str());
        return 0;
    }
    
    char str[100];
    int count = read(fd, str, sizeof(str));
    if (count == -1) return 0;
    str[count] = '\0';
    close(fd);
    int value = atoi(str);
    return value;
}

void setScreenBacklight(int percent) {
    int ret = 0;
    int time = 200; // ms
    int fades = 25; // steps to use for fading
    char cmd[100];
    int maxScreenBacklight = getScreenBacklightMax(); // could be static if we are sure if it will not be changed at program lifetime
    if (!maxScreenBacklight) {
        syslog(LOG_ERR, "Failed to get max screen backlight.");
        return;
    }
    int curScreenBacklight = getScreenBacklightCur();
    int newScreenBacklight = maxScreenBacklight*percent/100;
    int step = (newScreenBacklight - curScreenBacklight) / fades;
    int val = curScreenBacklight;

    string path = getScreenBacklightDevicePath() + "brightness";
    for (int i=0; i<fades; i++) {
	val += step;
        snprintf(cmd, sizeof(cmd), "echo %d | tee %s", val, path.c_str());
        ret = system(cmd);
        if (ret < 0) {
            syslog(LOG_ERR, "Failed to set screen backlight.");
        }
	usleep(time*1000/fades);
    }
}

void setKeyboardBacklight(int percent) {
    int value = 0;
    int ret = 0;
    if(percent <= 25) value = 0;
    else if(percent <= 50) value = 1;
    else if(percent <= 75) value = 2;
    else if(percent <= 100) value = 3;

    char cmd[150];
    snprintf(cmd, 150, "echo %d | tee /sys/class/leds/asus::kbd_backlight/brightness", value);
    // FIXME system() is not reliable when running with setuid.
    // Use execl() instead (see setScreenBacklight())
    ret = system(cmd);
    if (ret < 0) {
        syslog(LOG_ERR, "Failed to set keyboard backlight.");
    }
}

/**
 * @brief getLidStatus
 * @return 1 if opened, 0 if closed, -1 on error, -2 if unknown
 */
int getLidStatus() {
    int fd = open("/proc/acpi/button/lid/LID/state", O_RDONLY);
    if(fd == -1) {
        syslog(LOG_ERR, "Error opening /proc/acpi/button/lid/LID/state");
        return -1;
    } else {
        char str[100];
        int count = read(fd, str, 100);
	if (count == -1) return -1;
        str[count] = '\0';
        close(fd);
        string s = string(str);
        if(s.find("open") != string::npos) {
            return 1;
        } else if(s.find("closed") != string::npos) {
            return 0;
        } else {
            return -2;
        }
    }
}

int getAmbientLightPercent() {
    int fd = open("/sys/bus/acpi/devices/ACPI0008:00/ali", O_RDONLY);
    if(fd == -1) {
        logServerExit(EXIT_FAILURE, LOG_CRIT, "Error opening /sys/bus/acpi/devices/ACPI0008:00/ali");
    }
    char strals[100];
    int count = read(fd, strals, 100);
    if (count == -1) {
        logServerExit(EXIT_FAILURE, LOG_CRIT, "Error reading /sys/bus/acpi/devices/ACPI0008:00/ali");
    }
    strals[count] = '\0';
    close(fd);

    // XXX: According to ACPI specs _ALI's unit should be lux aka lumen/m^2 (1-dark room, 10 dim conference room, 300-400 office, 1000 overcast day, 10000 bright day)
    // XXX: http://www.acpi.info/DOWNLOADS/ACPI_5_Errata%20A.pdf page 467+
    // XXX: My device returns values up to 25000, 0 means too dark to measure, -1 means too bright to measure
    // 0x32 (min illuminance), 0xC8, 0x190, 0x258, 0x320 (max illuminance).
    int als = atoi(strals);
    //printf("\"%s\"\n", strals);
    //printf("Illuminance detected: %d\n", als);

    float percent = 0;

    if (als < 1000 && als >=0) {
	    percent = int((als*100/1000)+0.5);
    } else {
	    percent = 100;
    }

    return percent;
}

int getAmbientLightLux() {
    int fd = open("/sys/bus/acpi/devices/ACPI0008:00/ali", O_RDONLY);
    if(fd == -1) {
        logServerExit(EXIT_FAILURE, LOG_CRIT, "Error opening /sys/bus/acpi/devices/ACPI0008:00/ali");
    }
    char strals[100];
    int count = read(fd, strals, 100);
    if (count == -1) {
        logServerExit(EXIT_FAILURE, LOG_CRIT, "Error reading /sys/bus/acpi/devices/ACPI0008:00/ali");
    }
    strals[count] = '\0';
    close(fd);
    int als = atoi(strals);
    return als;
}

int main(int argc, char *argv[])
{
    if(argc > 1) {
        Client c = Client(argc, argv, SOCKET_PATH);
        c.Run();
        exit(EXIT_SUCCESS);
    }

    struct pidfh *pfh;
    pid_t otherpid;
    pfh = pidfile_open("/var/run/als-controller.pid", 0600, &otherpid);
    if (pfh == NULL) {
        if (errno == EEXIST) {
                    errx(EXIT_FAILURE, "Daemon already running, pid: %jd.",
                        (intmax_t)otherpid);
        }
        /* If we cannot create pidfile from other reasons, only warn. */
        warn("Cannot open or create pidfile");
    }

    if (daemon(0, 0) == -1) {
        warn("Cannot daemonize");
        pidfile_remove(pfh);
        exit(EXIT_FAILURE);
    }

    pidfile_write(pfh);

    /* Change the file mode mask */
    umask(0);

    /* Open the log file */
    openlog("als-controller", LOG_PID, LOG_DAEMON);

    startDaemon();
    pidfile_remove(pfh);
    return 0;
}

void startDaemon()
{
    syslog(LOG_NOTICE, "Started.");

    /* Signals blocked in all threads.
     * sigManager is the only thread responsible to catch signals */
    sigemptyset(&g_sigset);
    sigaddset(&g_sigset, SIGINT);
    sigaddset(&g_sigset, SIGTERM);
    if(pthread_sigmask(SIG_SETMASK, &g_sigset, NULL) != 0) {
        logServerExit(EXIT_FAILURE, LOG_CRIT, "Sigmask error.");
    }

    pthread_t sigthread;
    if(pthread_create(&sigthread, NULL, sigManager, NULL) != 0) {
        logServerExit(EXIT_FAILURE, LOG_CRIT, "Creating thread.");
    }


    pthread_t thread_id;
    int err = pthread_create(&thread_id, NULL, IPCHandler, NULL);
    if(err != 0) {
        syslog(LOG_CRIT, "Cannot create thread");
        exit(EXIT_FAILURE);
    }

    while(1) {

        pthread_mutex_lock(&mtx);
        while(!active) {
            pthread_cond_wait(&start, &mtx);
        }
        pthread_mutex_unlock(&mtx);


        if(getLidStatus() == 0) {
            setKeyboardBacklight(0);
        } else {
		/*
            float als = getAmbientLightPercent();
            //printf("Illuminance percent: %f\n", als);

            if(als <= 10) {
                setScreenBacklight(40);
                setKeyboardBacklight(100);
            } else if(als <= 25) {
                setScreenBacklight(60);
                setKeyboardBacklight(0);
            } else if(als <= 50) {
                setScreenBacklight(75);
                setKeyboardBacklight(0);
            } else if(als <= 75) {
                setScreenBacklight(90);
                setKeyboardBacklight(0);
            } else if(als <= 100) {
                setScreenBacklight(100);
                setKeyboardBacklight(0);
            }
	    */
            // [root@hisi ~]# echo "\_SB.ALS._ALR" > /proc/acpi/call
            // [root@hisi ~]# cat /proc/acpi/call
            // [[0x14, 0x0], [0x14, 0x19], [0x28, 0x32], [0x32, 0x64], [0x41, 0x96], [0x50, 0xc8], [0x64, 0x12c], [0x7d, 0x190], [0x9b, 0x1f4], [0xc3, 0x258], [0xf0, 0x2bc], [0x127, 0x320], [0x168, 0x384], [0x1b8, 0x3e8], [0x217, 0x4e2], [0x294, 0x5dc]]
	    // ACPI defines the tuples as (brightness in % relative to baseline at 300 lux, brightness in lux)
	    // However my _ALR values go far above 150% for relative brightness. I assume it might be the raw brightness value for the intel_backlight device
            int lux = getAmbientLightLux();
	    int percent = 100;

	    if (lux < 25) {
		    percent = 2;
	    } else if (lux < 50) {
		    percent = 4;
	    } else if (lux < 100) {
		    percent = 5;
	    } else if (lux < 150) {
		    percent = 6;
	    } else if (lux < 200) {
		    percent = 8;
	    } else if (lux < 300) {
		    percent = 10;
	    } else if (lux < 400) {
		    percent = 13;
	    } else if (lux < 500) {
		    percent = 16;
	    } else if (lux < 600) {
		    percent = 20;
	    } else if (lux < 700) {
		    percent = 25;
	    } else if (lux < 800) {
		    percent = 31;
	    } else if (lux < 900) {
		    percent = 38;
	    } else if (lux < 1000) {
		    percent = 46;
	    } else if (lux < 1250) {
		    percent = 57;
	    } else if (lux < 1500) {
		    percent = 70;
	    }
	    setScreenBacklight(percent);
        }

        sleep(3);
    }

    logServerExit(EXIT_SUCCESS, LOG_NOTICE, "Terminated.");
}

void *IPCHandler(void *arg)
{
    unlink(C_SOCKET_PATH);
    g_socket = createServerChannel(C_SOCKET_PATH);
    if(g_socket == -1) {
      logServerExit(EXIT_FAILURE, LOG_CRIT, "Error creating socket");
    }

    // Permessi 777 sulla socket
    if(chmod(C_SOCKET_PATH, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != 0) {
      closeServerChannel(C_SOCKET_PATH, g_socket);
      return NULL;
    }

    while(1)
    {
        int client = acceptConnection(g_socket);
        if(client == -1) {
            syslog(LOG_ERR, "Error accepting client connection.");
        } else {
            pthread_t thread_id;
            int err = pthread_create(&thread_id, NULL, clientHandler, (void *)(size_t)client);
            if(err != 0) {
                logServerExit(EXIT_FAILURE, LOG_CRIT, "Error creating client thread.");
            }
        }
    }
}

void *clientHandler(void *arg)
{
    int client = (int)(size_t)arg;
    message_t msg;

    if(receiveMessage(client, &msg) == -1) {
        syslog(LOG_ERR, "Error receiving message from client.");
        return NULL;
    }

    if(msg.type == MSG_ENABLE) {
        enableALS(true);
        pthread_mutex_lock(&mtx);
        active = true;
        pthread_mutex_unlock(&mtx);
        pthread_cond_signal(&start);
    } else if(msg.type == MSG_DISABLE) {
        pthread_mutex_lock(&mtx);
        active = false;
        pthread_mutex_unlock(&mtx);
        enableALS(false);
    } else if(msg.type == MSG_STATUS) {
        bool status = false;
        pthread_mutex_lock(&mtx);
        status = active;
        pthread_mutex_unlock(&mtx);

        int sent;
        message_t msg;

        msg.type = status ? MSG_ENABLED : MSG_DISABLED;
        msg.buffer = NULL;
        msg.length = 0;

        sent = sendMessage(client, &msg);
        if(sent == -1) {
            syslog(LOG_ERR, "Error sending reply to client.");
            return NULL;
        }
    }

    return NULL;
}
