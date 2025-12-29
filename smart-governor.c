#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

#define CONFIG_FILE "/etc/smart-governor.conf"
#define PID_FILE "/var/run/smart-governor.pid"

typedef struct {
    long long user;
    long long nice;
    long long system;
    long long idle;
    long long iowait;
    long long irq;
    long long softirq;
} CpuStats;

typedef struct {
    int poll_interval_secs;
    double cpu_high_threshold;
    double cpu_low_threshold;
    int temp_high_threshold_c;
    char performance_governor[32];
    char powersave_governor[32];
} Config;

volatile sig_atomic_t keep_running = 1;

void signal_handler(int signum) {
    syslog(LOG_INFO, "Signal %d received, shutting down.", signum);
    keep_running = 0;
}

void load_default_config(Config *config) {
    config->poll_interval_secs = 2;
    config->cpu_high_threshold = 60.0;
    config->cpu_low_threshold = 25.0;
    config->temp_high_threshold_c = 85;
    strcpy(config->performance_governor, "performance");
    strcpy(config->powersave_governor, "schedutil");
}

int get_user_config_gui(Config *config) {
    char cmd[1024];

    snprintf(cmd, sizeof(cmd),
        "zenity --forms --title='Smart Governor Daemon' "
        "--text='Current Defaults: High=%.0f%%, Low=%.0f%%, Temp=%dC\n\nEnter NEW values below (leaving blank may cause errors):' "
        "--add-entry='CPU High Threshold (%%)' "
        "--add-entry='CPU Low Threshold (%%)' "
        "--add-entry='Max Temp Threshold (C)' "
        "--separator='|'",
        config->cpu_high_threshold, config->cpu_low_threshold, config->temp_high_threshold_c);

    // Open the dialog
    FILE *fp = popen(cmd, "r");
    if (fp == NULL) {
        perror("Failed to run GUI dialog");
        return -1;
    }

    char result[128];
    if (fgets(result, sizeof(result), fp) != NULL) {
        double new_high, new_low;
        int new_temp;
        // Parse the pipe-separated output.
        // If user leaves a box blank, sscanf might fail, preserving defaults.
        if (sscanf(result, "%lf|%lf|%d", &new_high, &new_low, &new_temp) == 3) {
             config->cpu_high_threshold = new_high;
             config->cpu_low_threshold = new_low;
             config->temp_high_threshold_c = new_temp;
        }
    }

    int status = pclose(fp);
    if (status != 0) {
        printf("GUI cancelled. Using default/saved settings.\n");
        return -1;
    }
    return 0;
}
void parse_config(Config *config) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp == NULL) {
        load_default_config(config);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char key[64], value[64];
        if (sscanf(line, "%63s = %63s", key, value) == 2) {
            if (strcmp(key, "poll_interval_secs") == 0) config->poll_interval_secs = atoi(value);
            else if (strcmp(key, "cpu_high_threshold") == 0) config->cpu_high_threshold = atof(value);
            else if (strcmp(key, "cpu_low_threshold") == 0) config->cpu_low_threshold = atof(value);
            else if (strcmp(key, "temp_high_threshold_c") == 0) config->temp_high_threshold_c = atoi(value);
            else if (strcmp(key, "performance_governor") == 0) strncpy(config->performance_governor, value, 31);
            else if (strcmp(key, "powersave_governor") == 0) strncpy(config->powersave_governor, value, 31);
        }
    }
    fclose(fp);
}

int get_cpu_stats(CpuStats *stats) {
    FILE *fp = fopen("/proc/stat", "r");
    if (fp == NULL) {
        syslog(LOG_ERR, "Error opening /proc/stat: %s", strerror(errno));
        return -1;
    }
    fscanf(fp, "cpu %lld %lld %lld %lld %lld %lld %lld", &stats->user, &stats->nice, &stats->system, &stats->idle, &stats->iowait, &stats->irq, &stats->softirq);
    fclose(fp);
    return 0;
}

void set_governor_all_cpus(const char* governor) {
    DIR *dir = opendir("/sys/devices/system/cpu");
    if (!dir) {
        syslog(LOG_ERR, "Cannot open /sys/devices/system/cpu directory.");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cpu", 3) == 0 && isdigit(entry->d_name[3])) {
            char path[512];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/%s/cpufreq/scaling_governor", entry->d_name);
            
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "%s", governor);
                fclose(fp);
            }
        }
    }
    closedir(dir);
}

int get_max_temp() {
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) return -1;

    int max_temp = -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", entry->d_name);

            FILE *fp = fopen(path, "r");
            if (fp) {
                int temp;
                if (fscanf(fp, "%d", &temp) == 1) {
                     temp /= 1000;
                     if (temp > max_temp) {
                         max_temp = temp;
                     }
                }
                fclose(fp);
            }
        }
    }
    closedir(dir);
    return max_temp;
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    chdir("/");

    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }

    openlog("smart-governor", LOG_PID, LOG_DAEMON);
}

int main() {
    Config config;
    CpuStats prev_stats, curr_stats;
    char current_governor[32] = "";

    // --- MODIFICATION: Add counters for time tracking ---
    long long total_seconds_in_powersave = 0;
    long long total_seconds_in_performance = 0;
    long long total_seconds_unmanaged = 0; // Time before first governor is set
    // --- END MODIFICATION ---

    load_default_config(&config);
    parse_config(&config);

    printf("Launching configuration GUI...\n");
    if (get_user_config_gui(&config) == 0) {
        printf("Config configured via GUI: High: %.1f%%, Low: %.1f%%, Temp: %dC\n",
               config.cpu_high_threshold, config.cpu_low_threshold, config.temp_high_threshold_c);
    }

    if (geteuid() != 0) {
        fprintf(stderr, "ERROR: This program must be run as root to change CPU governors.\n");
        fprintf(stderr, "Try running with: sudo -E ./smart_governor\n");
        exit(EXIT_FAILURE);
    }

    printf("Starting daemon...\n");
    daemonize();

    int pid_file = open(PID_FILE, O_CREAT | O_RDWR, 0666);
    if (pid_file < 0 || lockf(pid_file, F_TLOCK, 0) < 0) {
        syslog(LOG_ERR, "Daemon already running or cannot open PID file.");
        exit(EXIT_FAILURE);
    }
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    write(pid_file, pid_str, strlen(pid_str));
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (get_cpu_stats(&prev_stats) != 0) {
        syslog(LOG_ERR, "Initial CPU stat read failed. Exiting.");
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Smart Governor Daemon started with thresholds: H=%.1f L=%.1f T=%d",
           config.cpu_high_threshold, config.cpu_low_threshold, config.temp_high_threshold_c);
    
    while (keep_running) {
        sleep(config.poll_interval_secs);
        
        if (get_cpu_stats(&curr_stats) != 0) continue;

        long long prev_idle_total = prev_stats.idle + prev_stats.iowait;
        long long curr_idle_total = curr_stats.idle + curr_stats.iowait;
        long long prev_active_total = prev_stats.user + prev_stats.nice + prev_stats.system + prev_stats.irq + prev_stats.softirq;
        long long curr_active_total = curr_stats.user + curr_stats.nice + curr_stats.system + curr_stats.irq + curr_stats.softirq;
        long long prev_total = prev_idle_total + prev_active_total;
        long long curr_total = curr_idle_total + curr_active_total;
        long long delta_total = curr_total - prev_total;
        long long delta_idle = curr_idle_total - prev_idle_total;

        double cpu_usage = 0.0;
        if (delta_total > 0) {
            cpu_usage = (double)(delta_total - delta_idle) / delta_total * 100.0;
        }

        int max_temp = get_max_temp();
        syslog(LOG_DEBUG, "Stats: CPU: %.2f%%, Temp: %dC", cpu_usage, max_temp);

        if (max_temp != -1 && max_temp > config.temp_high_threshold_c) {
            if (strcmp(current_governor, config.powersave_governor) != 0) {
                syslog(LOG_WARNING, "High Temp (%dC). Forcing %s.", max_temp, config.powersave_governor);
                set_governor_all_cpus(config.powersave_governor);
                strcpy(current_governor, config.powersave_governor);
            }
        } 
        else if (cpu_usage > config.cpu_high_threshold) {
            if (strcmp(current_governor, config.performance_governor) != 0) {
                syslog(LOG_INFO, "High Load (%.2f%%). Setting %s.", cpu_usage, config.performance_governor);
                set_governor_all_cpus(config.performance_governor);
                strcpy(current_governor, config.performance_governor);
            }
        } 
        else if (cpu_usage < config.cpu_low_threshold) {
             if (strcmp(current_governor, config.powersave_governor) != 0) {
                syslog(LOG_INFO, "Low Load (%.2f%%). Setting %s.", cpu_usage, config.powersave_governor);
                set_governor_all_cpus(config.powersave_governor);
                strcpy(current_governor, config.powersave_governor);
            }
        }

        prev_stats = curr_stats;

        // --- MODIFICATION: Add the elapsed poll time to the correct counter ---
        if (strcmp(current_governor, config.powersave_governor) == 0) {
            total_seconds_in_powersave += config.poll_interval_secs;
        } else if (strcmp(current_governor, config.performance_governor) == 0) {
            total_seconds_in_performance += config.poll_interval_secs;
        } else {
            // This captures time before a governor is set
            total_seconds_unmanaged += config.poll_interval_secs;
        }
        // --- END MODIFICATION ---
    }

    // --- MODIFICATION: Log the summary statistics before shutting down ---
    long long total_runtime = total_seconds_in_powersave + total_seconds_in_performance + total_seconds_unmanaged;
    double percent_powersave = 0.0;
    if (total_runtime > 0) {
        percent_powersave = (double)total_seconds_in_powersave / total_runtime * 100.0;
    }

    syslog(LOG_INFO, "--- Shutdown Statistics ---");
    syslog(LOG_INFO, "Total runtime: %lld seconds (~%.1f hours)", total_runtime, (double)total_runtime / 3600.0);
    syslog(LOG_INFO, "Time in %s: %lld seconds", config.powersave_governor, total_seconds_in_powersave);
    syslog(LOG_INFO, "Time in %s: %lld seconds", config.performance_governor, total_seconds_in_performance);
    syslog(LOG_INFO, "Time unmanaged: %lld seconds", total_seconds_unmanaged);
    syslog(LOG_INFO, "Est. resource saving (percent time in powersave): %.1f%%", percent_powersave);
    syslog(LOG_INFO, "---------------------------");
    // --- END MODIFICATION ---

    syslog(LOG_INFO, "Daemon shutting down.");
    set_governor_all_cpus("schedutil"); 
    closelog();
    remove(PID_FILE);
    close(pid_file);

    return 0;
}
