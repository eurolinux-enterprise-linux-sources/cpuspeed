/*
    $Id: cpuspeed.cc 7 2007-02-06 01:20:52Z carl $

    Copyright 2002 - 2008

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    cpuspeed.cc

    Author:
        Carl Thompson <cet@carlthompson.net>

    Contributors:
        Enrico Tassi <tassi@cs.unibo.it>

    This program is only for computers with Linux kernels compiled with
    CPUFreq.  You must have a CPU that supports frequency and/or voltage
    scaling via CPUFreq to use this program.  Your kernel must be compiled to
    support the "userspace" CPUFreq governor and the "sysfs" interface used
    by Linux 2.6.

    CPUSpeed no longer supports the "proc" interface used by Linux 2.4. If you
    need this please use version 1.2.x.

    I use this program on my Dell and HP laptops and netbooks to increase
    battery life and control performance.
*/

const char VERSION[] = "1.5";
const char AUTHOR[] = "Carl E. Thompson - cet [at] carlthompson.net (Copyright 2002 - 2008)";

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/errno.h>
#include <stdarg.h>
#include <time.h>
#include <sys/utsname.h>
#include <libgen.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/sysinfo.h>

// Maximum number of speed steps. Must be 2 or more. You don't need to change
// this. Even if your CPU supports more speed steps the program will use up to
// this number of them evenly spaced.
const unsigned MAX_SPEEDS  = 15;

// Minimum speed step supported in KHz
const unsigned MIN_STEP = 25000;

// Maximimum number of processor cores that can be controlled simultaneously
// by one CPUSpeed process
const unsigned MAX_TIED_CORES = 8;

const char SYSFS_CPUFREQ_DIR[] =
    "/sys/devices/system/cpu/cpu%u/cpufreq";

const char SYSFS_MIN_SPEED_FILE[] =
    "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_min_freq";

const char SYSFS_MAX_SPEED_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_max_freq";

const char SYSFS_CURRENT_SPEED_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_setspeed";

const char SYSFS_GOVERNOR_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_governor";

const char SYSFS_AFFECTED_CPUS_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/affected_cpus";

const char SYSFS_USERSPACE[] =
    "userspace";

const char * PROC_STAT_FILE =
    "/proc/stat";

#ifdef DEBUG
const char * FAKE_STAT_FILE =
    "fake_cpus/stat";

const char FAKE_CPUFREQ_DIR[] =
    "fake_cpus/%u";

const char FAKE_MIN_SPEED_FILE[] =
    "fake_cpus/%u/speed-min";

const char FAKE_MAX_SPEED_FILE[] =
    "fake_cpus/%u/speed-max";

const char FAKE_CURRENT_SPEED_FILE[] =
    "fake_cpus/%u/speed";

const char FAKE_GOVERNOR_FILE[] =
    "fake_cpus/%u/governor";

const char FAKE_AFFECTED_CPUS_FILE[] =
    "fake_cpus/%u/affected_cores";

const char FAKE_USERSPACE[] =
    "userspace";
#endif

const char * MIN_SPEED_FILE;
const char * MAX_SPEED_FILE;
const char * CURRENT_SPEED_FILE;
const char * GOVERNOR_FILE;
const char * AFFECTED_CPUS_FILE;
const char * USERSPACE;
const char * STAT_FILE;

// if CPU idle percentage is below this, CPU will be set to fastest speed
unsigned clock_up_idle_fast= 10;

// if CPU idle percentage is below this, CPU will be set to next higher speed
// if CPU idle percentage is above this, CPU will be set to next lower speed
unsigned idle_threshold = 25;

// if this is set (via command line) throttle down CPU to minimum if
// temperature is too high
const char * temperature_filename = 0;
unsigned max_temperature;

// if this is set (via command line) throttle down CPU to minimum if
// AC power is disconnected
const char * ac_filename = 0;

// if this is true then maximize speed when AC connected
bool max_speed_on_ac = false;

// if this is true then minimize speed when AC disconnected
bool min_speed_on_battery = true;

// if this is true then NICE time does not count as utilized time
bool nice_counts_as_idle = true;

// if this is true then IO wait time does not count as utilized time
bool io_counts_as_idle = true;

bool check_cpu = false, check_therm = false, check_ac = false;

// defines what info we care about for each speed step
static struct
{
    unsigned khz;
    //unsigned volts;
    //unsigned fsb;
} speeds[MAX_SPEEDS + 1];
unsigned current_speed; // current speed step
unsigned last_step; // lowest speed step

// which CPU cores are we controlling
unsigned tied_cpu_cores[MAX_TIED_CORES];
unsigned num_tied_cores = 0;

// display an error message and exit the program
void
die(bool system_error, const char *fmt, ...)
{
    fprintf(stderr, "Error: ");

    va_list ap;
    // get variable argument list passed
    va_start(ap, fmt);
    // display message passed on stderr
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
    if (system_error)
    {
        fprintf(stderr, "Error: %s\n", strerror(errno));
    }

    exit(1);
}

// read a line from a file
void
read_line(const char * filename, char * line, unsigned len)
{
    FILE * fp = fopen(filename, "r");
    if (!fp)
    {
        die(true, "Could not open file for reading: %s", filename);
    }
    if ( (!fgets(line, len, fp)) )
    {
        die(false, "Could not read from file: %s", filename);
    }
    fclose(fp);
    char * p = strchr(line, '\n');
    if (p)
    {
        *p = 0;
    }
}

// write a line to a file
void
write_line(const char * filename, const char *fmt, ...)
{
    FILE * fp = fopen(filename, "w+");
    if (!fp)
    {
        die(true, "Could not open file for writing: %s", filename);
    }
    va_list ap;
    // get variable argument list passed
    va_start(ap, fmt);
    if (vfprintf(fp, fmt, ap) < 0)
    {
        die(true, "Could not write to file: %s", filename);
    }
    va_end(ap);
    fclose(fp);
}

long
get_int(const char * s)
{
    char * end;
    long r = strtol(s, &end, 10);
    if (*end != '\0')
    {
        die(false, "Not an integer: [%s]", s);
    }
    if (errno == ERANGE)
    {
        die(false, "Number is out of range: %s", s);
    }
    return r;
}

// read an integer value from a file
unsigned
read_value(const char * filename)
{
    char line[256];
    read_line(filename, line, sizeof line);
    return (unsigned) get_int(line);
}

// Read multiple integer values from a string. Returns them in the array
// reference pointed to by "values" . Returns the number of values read.
unsigned
parse_values(const char * string, unsigned *  values, unsigned len)
{
    char line[256];
    char * svalue;
    unsigned num_found = 0;
    strncpy(line, string, sizeof line);
    line[sizeof line - 1] = '\0';
    for (
            svalue = strtok(line, " \t\n");
            svalue;
            svalue = strtok(NULL, " \t\n"), num_found++
        )
    {
        if (num_found == len)
        {
            die(
                false,
                "More than the maximum allowed %u values found in input: [%s]",
                len, string
             );
        }
        values[num_found] = (unsigned) get_int(svalue);
    }

    return num_found;
}

// Read multiple integer values from a file. Returns them in the array
// reference pointed to by "values" . Returns the number of values read.
unsigned
read_values(const char * filename, unsigned *  values, unsigned len)
{
    char line[256];
    read_line(filename, line, sizeof line);
    return parse_values(line, values, len);
}

// get the current CPU speed
unsigned get_speed()
{
    return read_value(CURRENT_SPEED_FILE);
}

// set the current CPU speed
void set_speed(unsigned value)
{
#ifdef DEBUG
    fprintf(
        stderr, "[core%u] Setting speed to: %uKHz\n", tied_cpu_cores[0], value
    );
#endif
    write_line(CURRENT_SPEED_FILE, "%u\n", value);
    // give CPU / chipset voltage time to settle down
    usleep(10000);
}

// This code smoothly transitions the CPU speed from 'current' to 'target'
// instead of jumping directly to the new speed because some AMD Mobile
// Athlon parts seem to choke on large differentials causing kernel panics.
void set_speed(unsigned current, unsigned target)
{
    if (current == target)
    {
        return;
    }
    int delta = (current > target) ? -1 : 1;
    do
    {
        current += delta;
        set_speed(speeds[current].khz);
    } while (current != target);
}

// comparison functions
inline unsigned MIN(unsigned a, unsigned b) { return a > b ? b : a; }
inline unsigned MAX(unsigned a, unsigned b) { return a > b ? a : b; }
inline int icomp(const void * a, const void * b)
    { return *((int *)a) - *((int *)b); }

// the minimum and maximum speed that we are allowed to set
unsigned min_speed = 0, max_speed = UINT_MAX;

// get the speed steps supported by the CPU
void
get_supported_speeds()
{
    unsigned min = read_value(MIN_SPEED_FILE);
    unsigned max = read_value(MAX_SPEED_FILE);

#ifdef DEBUG
    fprintf(
        stderr, "[core%u] Mimimum speed supported: %uKHz\n",
        tied_cpu_cores[0], min
    );
    fprintf(
        stderr, "[core%u] Maximum speed supported: %uKHz\n",
        tied_cpu_cores[0], max
    );
#endif
    min = MAX(min, min_speed);
    max = MIN(max, max_speed);

    unsigned step;
    const unsigned MAX_STEP = MIN_STEP << 8;
    for (step = MIN_STEP; step <= MAX_STEP; step *= 2)
    {
        min = MAX(min, step);
        if (max <= min)
        {
            die(false, "No speed steps could be determined!");
        }

        // go to max speed if we are not already there
        for (unsigned current = get_speed(); current < max; current += step)
        {
            set_speed(MIN(current, max));
        }
        set_speed(max);

        // this code is a hack to get the various speed steps supported by the
        // CPU by looping from the maximum speed to the minimum speed and
        // trying to set every possible speed divisible by step!

        // speed is set to max above so returns real maximum that can be set
        speeds[0].khz = get_speed();
        current_speed = 0;
        for (unsigned current = max-step; current > min-step; current -= step)
        {
            current = MAX(current, min);
            set_speed(current);
            unsigned real = get_speed();
            if (real != speeds[current_speed].khz)
            {
                speeds[++current_speed].khz = real;
                if (current_speed + 1 == MAX_SPEEDS)
                {
                    break;
                }
            }
        }
        if (current_speed + 1 != MAX_SPEEDS)
        {
            break;
        }
    }
    if (step > MAX_STEP)
    {
        die(false, "Detected more speed steps than this program can handle?!");
    }

    speeds[current_speed + 1].khz = 0;

    // the last step is the lowest found speed
    last_step = current_speed;

#ifdef DEBUG
    fprintf(stderr, "[core%u] Available speeds:\n", tied_cpu_cores[0]);
    for (unsigned speed = 0; speeds[speed].khz; speed++)
    {
        fprintf(
            stderr, "[core%u]  %2u: %9uKHz\n",
            tied_cpu_cores[0], speed, speeds[speed].khz
        );
    }
#endif
}

// are we currently dynamically scaling the CPU or at min or max?
enum Mode { SPEED_DYNAMIC, SPEED_MIN, SPEED_MAX } mode;

// gets the elapsed total time and elapsed idle time since it was last called
void
get_times(
    unsigned tied_core_index, unsigned long & total_elapsed,
    unsigned long & idle_elapsed
)
{
    FILE * fp = fopen(STAT_FILE, "r");
    if (!fp)
    {
        die(true, "Could not open %s for reading!", STAT_FILE);
    }

    static char search[MAX_TIED_CORES][8];
    static size_t searchlen[MAX_TIED_CORES];
    if (!searchlen[tied_core_index])
    {
        searchlen[tied_core_index] =
            snprintf(
                search[tied_core_index], sizeof search[tied_core_index],
                "cpu%u ", tied_cpu_cores[tied_core_index]
            );
#ifdef DEBUG
        fprintf(
            stderr, "[core%u] Looking for CPU line starting with: \"%s\"\n",
            tied_cpu_cores[tied_core_index], search[tied_core_index]
        );
#endif
    }
    bool found = false;
    char line[256];
    while (fgets(line, sizeof line, fp))
    {
        if (!strncmp(line, search[tied_core_index], searchlen[tied_core_index]))
        {
            found = true;
            break;
        }
    }

    fclose(fp);

    if (!found)
    {
#ifdef DEBUG
        fprintf(
            stderr, "[core%u] Could not find \'%s\' line in file: %s.\n",
            tied_cpu_cores[tied_core_index], search[tied_core_index], STAT_FILE
        );
#endif
        return;
    }

    char what[32];
    unsigned long user_time, nice_time, system_time, idle_time, wait_time = 0;
    sscanf(
        line, "%s %lu %lu %lu %lu %lu", what, &user_time, &nice_time,
        &system_time, &idle_time, &wait_time
    );

    // count nice time as idle time
    if (nice_counts_as_idle)
    {
        idle_time += nice_time;
    }

    // count IO wait time as idle time
    if (io_counts_as_idle)
    {
        idle_time += wait_time;
    }

    unsigned long total_time = user_time + system_time + idle_time;
    static unsigned long last_total_time[MAX_TIED_CORES],
        last_idle_time[MAX_TIED_CORES];

    total_elapsed = total_time - last_total_time[tied_core_index];
    last_total_time[tied_core_index] = total_time;
    idle_elapsed = idle_time - last_idle_time[tied_core_index];
    last_idle_time[tied_core_index] = idle_time;

#ifdef DEBUG
    fprintf(
        stderr, "[core%u] time: %lu    idle: %lu\n",
        tied_cpu_cores[tied_core_index], total_elapsed, idle_elapsed
    );
#endif
}

// resets the elapsed total time and elapsed idle time counters
void reset_times()
{
    unsigned long dummy1, dummy2;
    for (unsigned u = 0; u < num_tied_cores; u++)
    {
        get_times(u, dummy1, dummy2);
    }
}

// handles the periodic check of idle and setting CPU speed
void
alarm_handler(int)
{
    unsigned old_speed = current_speed;

    // current state
    Mode state = mode;

    static bool on_ac = true;

    char line[256], *p;

    if (mode == SPEED_DYNAMIC)
    {
        // check to see if AC power is disconnected
        if (ac_filename && check_ac)
        {
            read_line(ac_filename, line, sizeof line);
            if (strstr(line, "off-line"))
            {
                on_ac = false;
#ifdef DEBUG
                fprintf(stderr, "[core%u] AC is off-line\n", tied_cpu_cores[0]);
#endif
            }
            else
            {
                on_ac = true;
#ifdef DEBUG
                fprintf(stderr, "[core%u] AC is on-line\n", tied_cpu_cores[0]);
#endif
            }
        }

        if (max_speed_on_ac && on_ac)
        {
            state = SPEED_MAX;
        }
        else if (!on_ac && min_speed_on_battery)
        {
            state = SPEED_MIN;
        }

        // check that we are not getting too hot
        if (temperature_filename && check_therm && state != SPEED_MIN)
        {
            read_line(temperature_filename, line, sizeof line);
            p = strpbrk(line, "0123456789");
            if (!p)
            {
                die(
                    false, "Could not find temperature in file: %s",
                    temperature_filename
                );
            }
#ifdef DEBUG
            fprintf(
                stderr, "[core%u] temp: %ld\n", tied_cpu_cores[0],
                strtol(p, 0, 10)
            );
#endif
            if (strtol(p, 0, 10) > (long)max_temperature)
            {
                state = SPEED_MIN;
            }
        }
    }

    // figure out what our current speed should be
    switch(state)
    {
        case SPEED_DYNAMIC:
        {
            // if it's not yet time to check CPU then don't
            if (!check_cpu)
            {
                break;
            }

            unsigned wanted_speed[MAX_TIED_CORES];
            for (unsigned i = 0; i < num_tied_cores; i++)
            {
                unsigned long elapsed_time, idle_time;
                unsigned idle_percent;

                // get the elapsed and idle times since we last checked
                get_times(i, elapsed_time, idle_time);

                wanted_speed[i] = current_speed;
                if (elapsed_time > 0)
                {
                    idle_percent = idle_time * 100 / elapsed_time;

                    if (idle_percent <= clock_up_idle_fast)
                    {
                        wanted_speed[i] = 0;
                    }
                    else if (idle_percent < idle_threshold && current_speed > 0)
                    {
                        wanted_speed[i] = current_speed - 1;
                    }
                    else if (
                        idle_percent > idle_threshold
                        && speeds[current_speed + 1].khz != 0
                    )
                    {
                        wanted_speed[i] = current_speed + 1;
                    }
#ifdef DEBUG
                    fprintf(
                        stderr, "[core%u] idle percent: %.2u\n",
                        tied_cpu_cores[i], idle_percent
                    );
                    fprintf(
                        stderr, "[core%u] wanted speed: %u\n",
                        tied_cpu_cores[i], wanted_speed[i]
                    );
#endif
                }
            }
            qsort(wanted_speed, num_tied_cores, sizeof(unsigned), icomp);
            current_speed = wanted_speed[0];
#ifdef DEBUG
            fprintf(
                stderr, "[core%u] winning speed: %u\n",
                tied_cpu_cores[0], wanted_speed[0]
            );
#endif
            break;
        }

        case SPEED_MIN:
            current_speed = last_step;
            break;

        case SPEED_MAX:
            current_speed = 0;
            break;
    }

    // if the last set speed is not what it currently should be, set it
    if (current_speed != old_speed)
    {
#ifdef DEBUG
fprintf(stderr, "Current: %u\n", current_speed);
        fprintf(
            stderr, "[core%u] old speed: %uKHz     new speed: %uKHz\n",
            tied_cpu_cores[0], speeds[old_speed].khz, speeds[current_speed].khz
        );
#endif
        set_speed(old_speed, current_speed);
    }

#ifdef DEBUG
                fprintf(stderr, "\n");
#endif
    check_cpu = check_therm = check_ac = false;
}

// handles the USR1 signal (stay at maximum performance)
void
usr1_handler(int)
{
    mode = SPEED_MAX;
    raise(SIGALRM);
}

// handles the USR2 signal (stay at minimum performance)
void
usr2_handler(int)
{
    mode = SPEED_MIN;
    raise(SIGALRM);
}

// handles the HUP signal (dynamically scale performance)
void
hup_handler(int)
{
    reset_times();
    mode = SPEED_DYNAMIC;
    check_cpu = true;
    raise(SIGALRM);
}

unsigned num_cores = 0; // how many CPU cores are we managing? 0 = autodetect

// restore  initial speed on program exit
unsigned saved_speed = 0;
char saved_governor[32];

void
term_handler(int which)
{
    if (saved_speed)
    {
        set_speed(saved_speed);
        write_line(GOVERNOR_FILE, "%s\n", saved_governor);
    }

    raise(which);
}

char *
dup_cpu_str(const char *s)
{
    char buf[256];
    snprintf(buf, sizeof buf, s, tied_cpu_cores[0]);
    return strdup(buf);
}

int
main(int argc, char * argv[])
{
    const char * const NAME = basename(strdup(argv[0]));

    // intervals (in tenths of a second) at which we poll
    unsigned interval = 20; // 2 seconds
    unsigned therm_interval = 10; // 1 second
    unsigned ac_interval = 50; // 5 seconds

    bool daemonize = false; // should the process daemonize itself?
    bool save_state = false; // save current speed and restore on exit
#ifdef DEBUG
    bool fake_cpu = false; // use fake CPUs for testing
#endif

    // parse argv
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-d"))
            daemonize = true;
        else if (!strcmp(argv[i], "-r"))
            save_state = true;
        else if (!strcmp(argv[i], "-C"))
            max_speed_on_ac = true;
        else if (!strcmp(argv[i], "-D"))
            min_speed_on_battery = false;
        else if (!strcmp(argv[i], "-n"))
            nice_counts_as_idle = false;
        else if (!strcmp(argv[i], "-w"))
            io_counts_as_idle = false;
        else if (!strcmp(argv[i], "-i"))
        {
            if (argc <= i + 1)
            {
                die(
                    false, "The \'-i\' option must be followed by an interval "
                    "in tenths of a second"
                );
            }
            interval = get_int(argv[++i]);
#ifdef DEBUG
            fprintf(stderr, "CPU interval is %u\n", interval);
#endif
        }
        else if (!strcmp(argv[i], "-p"))
        {
            if (argc <= i + 2)
            {
                die(false, "The \'-p\' option must be followed by 2 integers");
            }
            clock_up_idle_fast = get_int(argv[++i]);
            idle_threshold = get_int(argv[++i]);
#ifdef DEBUG
            fprintf(
                stderr, "Triggers are %u %u\n",
                clock_up_idle_fast, idle_threshold
            );
#endif
        }
        else if (!strcmp(argv[i], "-t"))
        {
            if (argc <= i + 2)
            {
                die(
                    false, "The \'-t\' option must be followed by a filename "
                    "and a temperature"
                );
            }
            temperature_filename = argv[++i];
            max_temperature = get_int(argv[++i]);
        }
        else if (!strcmp(argv[i], "-a"))
        {
            if (argc <= i + 1)
            {
                die(false, "The \'-a\' option must be followed by a filename");
            }
            ac_filename = argv[++i];
        }
        else if (!strcmp(argv[i], "-m"))
        {
            if (argc <= i + 1)
            {
                die(
                    false, "The \'-m\' option must be followed by a minimum "
                    "speed in KHz"
                );
            }
            min_speed = get_int(argv[++i]);
#ifdef DEBUG
            fprintf(
                stderr, "Minimum speed allowed by user: %uKHz\n", min_speed
            );
#endif
        }
        else if (!strcmp(argv[i], "-M"))
        {
            if (argc <= i + 1)
            {
                die(
                    false, "The \'-M\' option must be followed by a maximum "
                    "speed in KHz"
                );
            }
            max_speed = get_int(argv[++i]);
#ifdef DEBUG
            fprintf(
                stderr, "Maximum speed allowed by user: %uKHz\n", max_speed
            );
#endif
        }
        else if (!strcmp(argv[i], "-T"))
        {
            if (argc <= i + 1)
            {
                die(
                    false, "The \'-T\' option must be followed by an interval "
                    "in tenths of a second"
                );
            }
            therm_interval = get_int(argv[++i]);
#ifdef DEBUG
            fprintf(stderr, "Thermal interval is %u\n", therm_interval);
#endif
        }
        else if (!strcmp(argv[i], "-A"))
        {
            if (argc <= i + 1)
            {
                die(
                    false, "The \'-A\' option must be followed by an interval "
                    "in tenths of a second"
                );
            }
            ac_interval = get_int(argv[++i]);
#ifdef DEBUG
            fprintf(stderr, "AC interval is %u\n", ac_interval);
#endif
        }
        else if (!strcmp(argv[i], "-S"))
        {
            if (argc <= i + 1)
            {
                die(
                    false, "The \'-s\' option must be followed by a list of "
                    "CPU cores"
                );
            }
            num_tied_cores = parse_values(argv[++i], tied_cpu_cores, MAX_TIED_CORES);

#ifdef DEBUG
            fprintf(stderr, "Managing only CPU core(s): %s\n", argv[i]);
#endif
        }
#ifdef DEBUG
        else if (!strcmp(argv[i], "-f"))
        {
            if (argc <= i + 1)
            {
                die(
                    false, "The \'-f\' option must be followed by the number "
                    "of fake CPU cores"
                );
            }
            num_cores = get_int(argv[++i]);
            fake_cpu = true;
            fprintf(stderr, "%u fake CPU core(s) detected\n", num_cores);
        }
#endif
        else
        {
            fprintf(
                stderr,
                "%s v%s\n"
                "\n"
                "This program monitors the system's idle percentage and reduces or raises the\n"
                "CPU cores\' clock speeds accordingly to minimize power usage when idle and\n"
                "maximize performance when needed. By default the program counts time used by\n"
                "nice()d programs and time used waiting for IO as idle time.\n"
                "\n"
                "The program may also optionally be configured to reduce the CPU cores\' clock\n"
                "speeds if the temperature gets too high or minimize their speeds if the\n"
                "computer's AC adapter is disconnected.\n"
                "\n"
                "By default this program will manage every CPU core found in the system.\n"
                "\n"
                "Usage: %s [Options]\n"
                "\n"
                "    Options:\n"
                "        -d\n"
                "            Tells the process to daemonize itself (run in background).\n"
                "\n"
                "        -i <interval>\n"
                "            Sets the interval between idle percentage tests and possible speed\n"
                "            changes in tenths of a second (default is 20).\n"
                "\n"
                "        -p <fast up> <threshold>\n"
                "            Sets the CPU core idle percentage thresholds. <fast up> is the idle\n"
                "            percentage below which a CPU will be set to the highest possible\n"
                "            speed. <threshold> is the idle percentage above which a CPU's\n"
                "            speed will be decreased and below which a CPU's speed will be\n"
                "            increased (defaults are 10 and 25).\n"
                "\n"
                "        -m <minimum speed>\n"
                "            Sets the minimum speed in KHz below which a CPU core won't be set.\n"
                "\n"
                "        -M <maximum speed>\n"
                "            Sets the maximum speed in KHz above which a CPU core won't be set.\n"
                "\n"
                "        -n\n"
                "            Do not treat niced programs as idle time.\n"
                "\n"
                "        -w\n"
                "            Do not treat time waiting for IO as idle time.\n"
                "\n"
                "        -t <temp file> <maxtemp>\n"
                "            Sets the ACPI temperature file and the temperature at which CPU\n"
                "            cores will be set to minimum speed.\n"
                "\n"
                "        -T <interval>\n"
                "            Sets the interval at which the temperature will be polled in\n"
                "            tenths  of a second (default is 10).\n"
                "            (Requires the \'-t\' option above.)\n"
                "\n"
                "        -a <AC file>\n"
                "            Sets the ACPI AC adapter state file and tells the program to set\n"
                "            the CPU cores to minimum speed when the AC adapter is disconnected.\n"
                "            (This is the default but is changeable by the \'-D\' option below).\n"
                "\n"
                "        -A <interval>\n"
                "            Sets the interval at which the AC adapter state will be polled in\n"
                "            tenths  of a second (default is 50).\n"
                "            (Requires the \'-a\' option above.)\n"
                "\n"
                "        -C\n"
                "            Run at maximum speed when AC adapter is connected.\n"
                "            (Requires the \'-a\' option above.)\n"
                "\n"
                "        -D\n"
                "            Do NOT force minimum speed when AC adapter is disconnected.\n"
                "            (Requires the \'-a\' option above.)\n"
                "\n"
                "        -r\n"
                "            Restores previous speed on program exit.\n"
                "\n"
                "        -S \"<CPU core 1> [[<CPU core 2>] ...]\"\n"
                "            Manage only a single group of CPU cores.  All of the specified\n"
                "            cores will controlled as a single group (locked to the same speed)\n"
                "            and are in the range 0 to n-1 where \'n\' is the total number of CPU\n"
                "            cores in the system.  Note that when specifying multiple cores the\n"
                "            list must be enclosed in quotes.  Without this option the program\n"
                "            creates copies of itself to manage every core of every CPU in the\n"
                "            system and automatically determines core groups. If you are running\n"
                "            on an old kernel and get an error message about not being able to\n"
                "            open an \"affected_cpus\" file then you must run this program\n"
                "            separately for each group of cores that must be controlled together\n"
                "            (which probably means for each physical CPU) and use this option.\n"
                "\n"
                "    To have a CPU core stay at the highest clock speed to maximize performance\n"
                "    send the process controlling that CPU core the SIGUSR1 signal.\n"
                 "\n"
                "    To have a CPU core stay at the lowest clock speed to maximize battery life\n"
                "    send the process controlling that CPU core the SIGUSR2 signal.\n"
                 "\n"
                "    To resume having a CPU core's clock speed dynamically scaled send the\n"
                "    process controlling that CPU core the SIGHUP signal.\n"
                 "\n"
                "Author:\n"
                "    %s\n"
                "\n"
                , NAME, VERSION, NAME, AUTHOR
            );
            exit(0);
        }
    }

    // set up signal handling to ignore SIGCHLD signals
    // uncommenting this could cause TERM signals to be sent to the wrong
    // processes on exit so we accept possible <defunct> processes
//     struct sigaction child_action;
//     sigemptyset(&child_action.sa_mask);
//     child_action.sa_flags = 0;
//     child_action.sa_handler = SIG_IGN;
//     sigaction(SIGCHLD, &child_action, 0);

    // get number of CPU cores
    if (!num_cores)
    {
        num_cores = sysconf(_SC_NPROCESSORS_CONF);

#ifdef DEBUG
        fprintf(stderr, "%u CPU core(s) detected.\n", num_cores);
#endif
    }

    // if cpu cores to control were not specified by the -s option then figure
    // out what cores to control by looking at the affected_cpus file in the
    //  cpufreq dir for each core
    if (!num_tied_cores)
    {
        // iterate through all cpu cores in main process
        for (unsigned i = 0, forked = 0; i < num_cores && !forked; i++)
        {
            // does this core do cpufreq?
            char cpufreq_dir[256];
            snprintf(cpufreq_dir, sizeof cpufreq_dir, SYSFS_CPUFREQ_DIR, i);
#ifdef DEBUG
            if (fake_cpu)
            {
                snprintf(cpufreq_dir, sizeof cpufreq_dir, FAKE_CPUFREQ_DIR, i);
            }
#endif
            if ( access(cpufreq_dir, F_OK) != 0 )
            {
                // this core does not have a cpufreq dir so doesn't do cpufreq
#ifdef DEBUG
                fprintf(
                    stderr, "[core%u] WARNING: Skipping core because CPUFreq "
                    "directory (%s) not found\n", i, cpufreq_dir
                );
#endif
                continue;
            }

            // get list of affected cpu cores that this core belongs to
            char acfn[256];
            snprintf(acfn, sizeof acfn, SYSFS_AFFECTED_CPUS_FILE, i);
#ifdef DEBUG
            if (fake_cpu)
            {
                snprintf(acfn, sizeof acfn, FAKE_AFFECTED_CPUS_FILE, i);
            }
#endif
            unsigned cores[MAX_TIED_CORES];
            int n = read_values(acfn, cores, MAX_TIED_CORES);

            // if we can't figure out the affected cores
            if (n == 0)
            {
                die(
                    false, "[core%u] Could not read affected cores from: %s",
                    i, acfn
                );
            }

            // if this cpu core is the master of a group of tied cores then
            // fork() a process to handle this core group
            if (cores[0] == i)
            {
#ifdef DEBUG
                fprintf(
                    stderr, "[core%u] This core is controlled by me.\n", i
                );
#endif
                // if this is not the first cpu core then fork()
                if ((i == 0) || (forked = !fork()))
                {
                    memcpy(tied_cpu_cores, cores, sizeof tied_cpu_cores);
                    num_tied_cores = n;
                }
            }
            else
            {
#ifdef DEBUG
                fprintf(
                    stderr, "[core%u] This core is controlled by core #%u\n",
                    i, cores[0]
                );
#endif
            }
        }
    }


#ifdef DEBUG
    fprintf(
        stderr, "[core%u] Cores controlled by this core:",
        tied_cpu_cores[0]
    );
    for (unsigned i = 0; i < num_tied_cores; i++)
    {
        fprintf(stderr, " %u", tied_cpu_cores[i]);
    }
    fprintf(stderr, "\n");
#endif

    if (num_tied_cores == 0)
    {
        die(false, "Could not find any CPUFreq controlled CPU cores to manage");
    }

    STAT_FILE = PROC_STAT_FILE;
    MIN_SPEED_FILE = dup_cpu_str(SYSFS_MIN_SPEED_FILE);
    MAX_SPEED_FILE = dup_cpu_str(SYSFS_MAX_SPEED_FILE);
    CURRENT_SPEED_FILE = dup_cpu_str(SYSFS_CURRENT_SPEED_FILE);
    GOVERNOR_FILE = dup_cpu_str(SYSFS_GOVERNOR_FILE);
    USERSPACE = SYSFS_USERSPACE;
#ifdef DEBUG
    if (fake_cpu)
    {
        STAT_FILE = FAKE_STAT_FILE;
        MIN_SPEED_FILE = dup_cpu_str(FAKE_MIN_SPEED_FILE);
        MAX_SPEED_FILE = dup_cpu_str(FAKE_MAX_SPEED_FILE);
        CURRENT_SPEED_FILE = dup_cpu_str(FAKE_CURRENT_SPEED_FILE);
        GOVERNOR_FILE = dup_cpu_str(FAKE_GOVERNOR_FILE);
        USERSPACE = FAKE_USERSPACE;
    }
#endif

    // save current speed if necessary
    if (save_state)
    {
        saved_speed = get_speed();
        read_line(GOVERNOR_FILE, saved_governor, sizeof saved_governor);
    }

   // use the userspace governor
    write_line(GOVERNOR_FILE, "%s\n", USERSPACE);

    if (access(CURRENT_SPEED_FILE, W_OK) < 0)
    {
        die(true, "Cannot write to speed control file: %s", CURRENT_SPEED_FILE);
    }

    // set up signal handling
    struct sigaction signal_action;

    // block all of our signals during each signal handler to avoid possible
    // race condition (not that it would really matter)
    sigemptyset(&signal_action.sa_mask);
    sigaddset(&signal_action.sa_mask, SIGALRM);
    sigaddset(&signal_action.sa_mask, SIGUSR1);
    sigaddset(&signal_action.sa_mask, SIGUSR2);
    sigaddset(&signal_action.sa_mask, SIGHUP);
    sigaddset(&signal_action.sa_mask, SIGTERM);
    sigaddset(&signal_action.sa_mask, SIGQUIT);
    sigaddset(&signal_action.sa_mask, SIGINT);
    signal_action.sa_flags = 0;

    // set the SIGALRM handler to our function
    signal_action.sa_handler = alarm_handler;
    sigaction(SIGALRM, &signal_action, 0);

    // set the SIGUSR1 handler to our function
    signal_action.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &signal_action, 0);

    // set the SIGUSR2 handler to our function
    signal_action.sa_handler = usr2_handler;
    sigaction(SIGUSR2, &signal_action, 0);

    // set the HUP handler to our function
    signal_action.sa_handler = hup_handler;
    sigaction(SIGHUP, &signal_action, 0);

    // set up signal handling for terminate signal
    struct sigaction term_action;

    // block all of our signals during each signal handler to avoid possible
    // race condition (not that it would really matter)
    sigemptyset(&term_action.sa_mask);
    sigaddset(&term_action.sa_mask, SIGALRM);
    sigaddset(&term_action.sa_mask, SIGUSR1);
    sigaddset(&term_action.sa_mask, SIGUSR2);
    sigaddset(&term_action.sa_mask, SIGHUP);
    sigaddset(&term_action.sa_mask, SIGTERM);
    sigaddset(&term_action.sa_mask, SIGQUIT);
    sigaddset(&term_action.sa_mask, SIGINT);
    term_action.sa_flags = SA_ONESHOT;

    // set the TERM handler to our function
    term_action.sa_handler = term_handler;
    sigaction(SIGTERM, &term_action, 0);
    sigaction(SIGQUIT, &term_action, 0);
    sigaction(SIGINT, &term_action, 0);

    // reset the speed steps
    get_supported_speeds();
    // reset the time counters
    reset_times();

    // we dynamically scale speed to start
    mode = SPEED_DYNAMIC;

    unsigned counter = 0;
    unsigned cpu_interval_timeout = 0;
    unsigned therm_interval_timeout = 0;
    unsigned ac_interval_timeout = 0;
    unsigned next_timeout, d;
    timespec timeout;

    // run in background if requested - CET - FIXME
    if (daemonize)
    {
        daemon(0, 0);
    }

    // main loop
    while (1)
    {
        if (counter == cpu_interval_timeout)
        {
            cpu_interval_timeout += interval;
            check_cpu = true;
        }
        next_timeout = cpu_interval_timeout;

        if (temperature_filename)
        {
            if (counter == therm_interval_timeout)
            {
                therm_interval_timeout += therm_interval;
                check_therm = true;
            }
            next_timeout = MIN(next_timeout, therm_interval_timeout);
        }

        if (ac_filename)
        {
            if (counter == ac_interval_timeout)
            {
                ac_interval_timeout += ac_interval;
                check_ac = true;
            }
            next_timeout = MIN(next_timeout, ac_interval_timeout);
        }

        if (check_cpu || check_ac || check_therm)
        {
            raise(SIGALRM);
        }

        d = next_timeout - counter;
        timeout.tv_sec = d / 10;
        timeout.tv_nsec = (d % 10) * 100000000LU;
        while (nanosleep(&timeout, &timeout) == -1 && errno == EINTR)
        {} // loop just makes sure all of timeout has passed

        counter = next_timeout;
    }

    // never reached
    return 0;
}

/* $Id: cpuspeed.cc 7 2007-02-06 01:20:52Z carl $ */
