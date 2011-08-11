/* **************************************************************
Copyright (C) 2010 Hewlett-Packard Development Company, L.P.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
************************************************************** */

/* local includes */
#include <libfossrepo.h>
#include <agent.h>
#include <database.h>
#include <event.h>
#include <host.h>
#include <interface.h>
#include <logging.h>
#include <scheduler.h>

#include <fossconfig.h>

/* std library includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* unix system includes */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

/* glib includes */
#include <glib.h>
#include <gio/gio.h>

#define AGENT_CONF "mods-enabled"
#ifndef PROCESS_NAME
#define PROCESS_NAME "fo_scheduler"
#endif

#define TEST_ERROR(...)                                            \
  if(error)                                                        \
  {                                                                \
    lprintf("ERROR %s.%d: %s\n",                                   \
      __FILE__, __LINE__, error->message);                         \
    lprintf("ERROR %s.%d: ", __FILE__, __LINE__);                  \
    lprintf(__VA_ARGS__);                                          \
    lprintf("\n");                                                 \
    error = NULL;                                                  \
    continue;                                                      \
  }                                                                \

/* global flags */
int verbose = 0;
int closing = 0;
int s_pid;
int s_daemon;
int s_port;

/* ************************************************************************** */
/* **** signals and events ************************************************** */
/* ************************************************************************** */

/**
 * handle signals from the child process. This will only be called on a SIGCHLD
 * and will handle the effects of the death of the child process.
 *
 * @param signo
 * @param INFO
 * @param context
 */
void chld_sig(int signo)
{
  int idx;          // index of the next empty location in the pid list
  pid_t* pid_list;  // list of dead agent pid's
  pid_t n;          // the next pid that has died
  int status;       // status returned by waitpit()

  /* initialize memory */
  pid_list = g_new0(pid_t, num_agents() + 1);
  idx = 0;

  /* get all of the dead children's pids */
  while((n = waitpid(-1, &status, WNOHANG)) > 0)
  {
    if(TVERBOSE2)
      clprintf("SIGNALS: received sigchld for pid %d\n", n);
    pid_list[idx++] = n;
  }

  event_signal(agent_death_event, pid_list);
}

/**
 * Handles any signals sent to the scheduler that are not SIGCHLD.
 *
 * Currently Handles:
 *   SIGALRM: scheduler will run agent updates and database updates
 *   SIGTERM: scheduler will gracefully shut down
 *   SIGQUIT: scheduler will gracefully shut down
 *   SIGINT:  scheduler will gracefully shut down
 *   SIGHIP:  scheduler will reload configuration data
 *
 * @param signo the number of the signal that was sent
 */
void prnt_sig(int signo)
{
  switch(signo)
  {
    case SIGALRM:
      lprintf("SIGNALS: Scheduler received alarm signal, checking job states\n");
      event_signal(agent_update_event, NULL);
      event_signal(database_update_event, NULL);
      alarm(CHECK_TIME);
      break;
    case SIGTERM:
      lprintf("SIGNALS: Scheduler received terminate signal, shutting down scheduler\n");
      event_signal(scheduler_close_event, NULL);
      break;
    case SIGQUIT:
      lprintf("SIGNALS: Scheduler received quit signal, shutting down scheduler\n");
      event_signal(scheduler_close_event, NULL);
      break;
    case SIGINT:
      lprintf("SIGNALS: Scheduler received interrupt signal, shutting down scheduler\n");
      event_signal(scheduler_close_event, NULL);
      break;
    case SIGHUP:
      load_config(NULL);
      break;
  }
}

/* ************************************************************************** */
/* **** The actual scheduler ************************************************ */
/* ************************************************************************** */

/**
 * The heart of the scheduler, the actual scheduling algorithm. This will be
 * passed to the event loop as a call back and will be called every time an event
 * is executed. Therefore the code should be light weight since it will be run
 * very frequently.
 *
 * TODO:
 *   currently this will only grab a job and create a single agent to execute
 *   the job.
 *
 *   TODO: allow for runonpfile jobs to have multiple agents based on size
 *   TODO: allow for job preemption. The scheduler can pause jobs, allow it
 *   TODO: allow for specific hosts to be chossen.
 */
void update_scheduler()
{
  /* queue used to hold jobs if an exclusive job enters the system */
  static job j = NULL;
  static int lockout = 0;

  /* locals */
  int n_agents = num_agents();
  int n_jobs   = active_jobs();

  if(closing && n_agents == 0 && n_jobs == 0)
  {
    event_loop_terminate();
    return;
  }

  if(lockout && n_agents == 0 && n_jobs == 0)
    lockout = 0;

  if(j == NULL && !lockout)
  {
    while((j = next_job()) != NULL)
    {
      if(is_exclusive(job_type(j)))
        break;

      // TODO handle no available host
      //if((h = get_host(1)) == NULL)
      //  continue;

      agent_init(get_host(1), j, 0);
    }
  }

  if(j != NULL && n_agents == 0 && n_jobs == 0)
  {
    agent_init(get_host(1), j, 0);
    lockout = 1;
    j = NULL;
  }
}

/* ************************************************************************** */
/* **** main utility functions ********************************************** */
/* ************************************************************************** */

/**
 * TODO
 *
 * @return
 */
int unlock_scheduler()
{
  return shm_unlink(PROCESS_NAME);
}

/**
 * TODO
 *
 * @return
 */
pid_t get_locked_pid()
{
  pid_t pid = 0;
  ssize_t bytes;
  int handle, rc;
  char buf[10];

  /* Initialize memory */
  handle = rc = 0;
  memset(buf, '\0', sizeof(buf));

  /* open the shared memory */
  if((handle = shm_open(PROCESS_NAME, O_RDONLY, 0444)) < 0)
  {
    if(errno != ENOENT)
      ERROR("failed to acquire shared memory", PROCESS_NAME);
    return 0;
  }

  /* find out who owns the shared memory */
  bytes = read(handle, buf, sizeof(buf));
  if((pid = atoi(buf)) < 2)
  {
    if(shm_unlink(PROCESS_NAME) == -1)
      ERROR("failed to remove invalid lock");
    return 0;
  }

  /* check to see if the pid is a valid process */
  if(kill(pid, 0) == 0)
    return pid;

  unlock_scheduler();
  return 0;
}

/**
 * TODO
 *
 * @return
 */
pid_t lock_scheduler()
{
  pid_t pid;
  int handle;
  char buf[10];

  /* return if lock already exists */
  if((pid = get_locked_pid()))
    return pid;

  /* no lock, create a new lock file */
  if((handle = shm_open(PROCESS_NAME, O_RDWR|O_CREAT|O_EXCL, 0744)) == -1)
  {
    ERROR("failed to open shared memory");
    return -1;event_signal(database_update_event, NULL);
  }

  sprintf(buf, "%-9.9d", getpid());
  if(write(handle, buf, sizeof(buf)) < 1)
  {
    ERROR("failed to write pid to lock file");
    return -1;
  }

  return 0;
}

/**
 * Correctly set the project user and group. The fossology scheduler must run as
 * the user specified by PROJECT_USER and PROJECT_GROUP since the agents must be
 * able to connect to the database. This ensures that that happens correctly.
 */
void set_usr_grp()
{
  /* locals */
  struct group*  grp;
  struct passwd* pwd;

  /* make sure group exists */
  grp = getgrnam(PROJECT_GROUP);
  if(!grp)
  {
    // TODO error message
  }

  /* set the project group */
  setgroups(1, &(grp->gr_gid));
  if((setgid(grp->gr_gid) != 0) || (setegid(grp->gr_gid) != 0))
  {event_signal(database_update_event, NULL);
    fprintf(stderr, "FATAL %s.%d: %s must be run as root or %s\n", __FILE__, __LINE__, PROCESS_NAME, PROJECT_USER);
    fprintf(stderr, "FATAL Set group '%s' aborting due to error: %s\n", PROJECT_GROUP, strerror(errno));
    exit(-1);
  }

  /* run as project user */
  pwd = getpwnam(PROJECT_USER);
  if(!pwd)
  {
    fprintf(stderr, "FATAL %s.%d: user '%s' not found\n", __FILE__, __LINE__, PROJECT_USER);
    exit(-1);
  }

  /* run as correct user, not as root or any other user */
  if((setuid(pwd->pw_uid) != 0) || (seteuid(pwd->pw_uid) != 0))
  {
    fprintf(stderr, "FATAL %s.%d: %s must run this as %s\n", __FILE__, __LINE__, PROCESS_NAME, PROJECT_USER);
    fprintf(stderr, "FATAL SETUID aborting due to error: %s\n", strerror(errno));
    exit(-1);
  }
}

/**
 * TODO
 */
void kill_scheduler()
{
  pid_t pid;

  if((pid = get_locked_pid()))
  {
    if(kill(pid, SIGQUIT) == -1)
    {
      ERROR("Unable to send SIGQUIT to PID %d", pid);
      return;
    }
    else
    {
      fprintf(stderr, "Exiting %s PID %d\n", PROCESS_NAME, pid);
      lprintf(        "Exiting %s PID %d\n", PROCESS_NAME, pid);
    }

    unlock_scheduler();
  }
}

/**
 * TODO
 */
void load_agent_config()
{
  DIR* dp;                  // directory pointer used to load meta agents;
  struct dirent* ep;        // information about directory
  char addbuf[256];         // standard string buffer
  int max = -1;             // the number of agents to a host or number of one type running
  int special = 0;          // anything that is special about the agent (EXCLUSIVE)
  int i;
  char* name;
  char* cmd;
  char* tmp;
  GError* error = NULL;

  /* clear previous configurations */
  agent_list_clean();

  snprintf(addbuf, sizeof(addbuf), "%s/%s/", DEFAULT_SETUP, AGENT_CONF);
  if((dp = opendir(addbuf)) == NULL)
  {
    FATAL("Could not open agent config directory: %s", addbuf);
    return;
  }

  /* load the configuration for the agents */
  while((ep = readdir(dp)) != NULL)
  {
    if(ep->d_name[0] != '.')
    {
      sprintf(addbuf, "%s/%s/%s/%s.conf",
          DEFAULT_SETUP, AGENT_CONF, ep->d_name, ep->d_name);

      fo_config_load(addbuf, &error);
      if(error && error->code == fo_missing_file)
      {
        VERBOSE3("CONFIG: Could not find %s\n", addbuf);
        g_error_free(error);
        error = NULL;
        continue;
      }
      TEST_ERROR("no additional info");
      VERBOSE2("CONFIG: loading config file %s\n", addbuf);

      if(!fo_config_has_group("default"))
      {
        lprintf("ERROR: %s must have a \"default\" group\n", addbuf);
        lprintf("ERROR: cause by %s.%d\n", __FILE__, __LINE__);
        error = NULL;
        continue;
      }

      special = 0;
      max = fo_config_list_length("default", "special", &error);
      TEST_ERROR("the special key should be of type list");
      for(i = 0; i < max; i++)
      {
        cmd = fo_config_get_list("default", "special", i, &error);
        TEST_ERROR("failed to load element %d of special list", i)
        if(strcmp(cmd, "EXCLUSIVE") == 0)
          special |= SAG_EXCLUSIVE;
      }

      name = fo_config_get("default", "name", &error);
      TEST_ERROR("the default group must have a name key");
      cmd  = fo_config_get("default", "command", &error);
      TEST_ERROR("the default group must have a command key");
      tmp  = fo_config_get("default", "max", &error);
      TEST_ERROR("the default group must have a max key");

      if(!add_meta_agent(name, cmd, atoi(tmp), special))
      {
        VERBOSE2("CONFIG: could not create meta agent using %s\n", ep->d_name);
      }
      else if(TVERBOSE2)
      {
        lprintf("CONFIG: added new agent\n");
        lprintf("    name = %s\n", name);
        lprintf(" command = %s\n", cmd);
        lprintf("     max = %d\n", max);
        lprintf(" special = %d\n", special);
        lprintf("CONFIG: will use \"");
        lprintf(AGENT_BINARY, AGENT_DIR, name, cmd);
        lprintf("\"\n");
      }
    }
  }
  closedir(dp);
  for_each_host(test_agents);
}

/**
 * TODO
 */
void load_foss_config()
{
  char* tmp;                // pointer into a string
  char** keys;
  int max = -1;             // the number of agents to a host or number of one type running
  int special = 0;          // anything that is special about the agent (EXCLUSIVE)
  char addbuf[256];         // standard string buffer
  char dirbuf[256];         // standard string buffer
  GError* error = NULL;
  int i;

  /* clear all previous configurations */
  host_list_clean();

  /* parse the config file */
  fo_config_load_default(&error);
  if(error)
    FATAL("%s", error->message);

  /* load the port setting */
  if(s_port < 0)
    s_port = atoi(fo_config_get("FOSSOLOGY", "port", &error));
  set_port(s_port);

  /* load the host settings */
  keys = fo_config_key_set("HOSTS", &special);
  for(i = 0; i < special; i++)
  {
    tmp = fo_config_get("HOSTS", keys[i], &error);
    if(error)
    {
      lprintf(error->message);
      error = NULL;
      continue;
    }

    sscanf(tmp, "%s %s %d", addbuf, dirbuf, &max);
    if(strcmp(addbuf, "localhost") == 0) strcpy(dirbuf, AGENT_DIR);

    host_init(keys[i], addbuf, dirbuf, max);
    if(TVERBOSE2)
    {
      lprintf("CONFIG: added new host\n");
      lprintf("      name = %s\n", keys[i]);
      lprintf("   address = %s\n", addbuf);
      lprintf(" directory = %s\n", dirbuf);
      lprintf("       max = %d\n", max);
    }
  }
}

/**
 * TODO
 *
 * @param unused
 */
void load_config(void* unused)
{
  load_foss_config();
  load_agent_config();
}

/**
 * TODO
 *
 * @param unused
 */
void scheduler_close_event(void* unused)
{
  closing = 1;
  kill_agents();
}

/**
 * TODO
 *
 * @return
 */
int close_scheduler()
{
  job_list_clean();
  host_list_clean();
  agent_list_clean();
  interface_destroy();
  database_destroy();
  event_loop_destroy();
  fo_RepClose();
  return 0;
}

/**
 * Utility function that enables the use of the strcmp function with a GTree.
 *
 * @param a The first string
 * @param b The second string
 * @param user_data unused in this function
 * @return integral value idicating the relatioship between the two strings
 */
gint string_compare(gconstpointer a, gconstpointer b, gpointer user_data)
{
  return strcmp((char*)a, (char*)b);
}

/**
 * Utility function that enable the agents to be stored in a GTree using
 * the PID of the associated process.
 *
 * @param a The pid of the first process
 * @param b The pid of the second process
 * @param user_data unused in this function
 * @return integral value idicating the relationship between the two pids
 */
gint int_compare(gconstpointer a, gconstpointer b, gpointer user_data)
{
  return *(int*)a - *(int*)b;
}

/* ************************************************************************** */
/* **** main types ********************************************************** */
/* ************************************************************************** */

/**
 * main function for FOSSology scheduler, does command line parsing,
 * Initialization and then simply enters the event loop.
 *
 * @param argc the command line argument cound
 * @param argv the command line argument values
 * @return if the program ran correctly
 */
int main(int argc, char** argv)
{
  /* locals */
  gboolean db_reset = FALSE;  // flag to reset the job queue upon database connection
  gboolean ki_sched = FALSE;  // flag that indicates that the scheduler will be killed after start
  gboolean db_init  = FALSE;  // flag indicating a database test
  gboolean test_die = FALSE;  // flag to run the tests then die
  char* log = NULL;           // used when a different log from the default is used
  GOptionContext* options;    // option context used for command line parsing
  GError* error = NULL;       // error object used during parsing
  int rc;                     // used for return values of

  /* the options for the command line parser */
  GOptionEntry entries[] =
  {
      { "daemon",   'd', 0, G_OPTION_ARG_NONE,   &s_daemon, "Run scheduler as daemon"                     },
      { "database", 'i', 0, G_OPTION_ARG_NONE,   &db_init,  "Initialize database connection and exit"     },
      { "kill",     'k', 0, G_OPTION_ARG_NONE,   &ki_sched, "Kills all running schedulers and exit"       },
      { "log",      'L', 0, G_OPTION_ARG_STRING, &log,      "Prints log here instead of default log file" },
      { "port",     'p', 0, G_OPTION_ARG_INT,    &s_port,   "Set the port the interface listens on"       },
      { "reset",    'R', 0, G_OPTION_ARG_NONE,   &db_reset, "Reset the job queue upon startup"            },
      { "test",     't', 0, G_OPTION_ARG_NONE,   &test_die, "Close the scheduler after running tests"     },
      { "verbose",  'v', 0, G_OPTION_ARG_INT,    &verbose,  "Set the scheduler verbose level"             },
      {NULL}
  };

  /* make sure port is correctly initialized */
  s_pid = getpid();
  s_daemon = FALSE;
  s_port = -1;

  /* ********************* */
  /* *** parse options *** */
  /* ********************* */
  options = g_option_context_new("- scheduler for FOSSology");
  g_option_context_add_main_entries(options, entries, NULL);
  g_option_context_parse(options, &argc, &argv, &error);

  if(error)
  {
    fprintf(stderr, "ERROR: %s\n", error->message);
    fprintf(stderr, "%s", g_option_context_get_help(options, FALSE, NULL));
    fflush(stderr);
    return -1;
  }

  g_option_context_free(options);

  /* make sure we are running as fossy */
  set_usr_grp();

  /* perform pre-initialization checks */
  if(s_daemon) { rc = daemon(0, 0); }
  if(db_init) { database_init(); return 0; }
  if(ki_sched) { kill_scheduler(); return 0; }
  if(log != NULL) {set_log(log); }

  if(lock_scheduler() <= 0 && !get_locked_pid())
    FATAL("scheduler lock error");

  /* ********************************** */
  /* *** do all the initializations *** */
  /* ********************************** */
  g_thread_init(NULL);
  g_type_init();
  fo_RepOpen();
  agent_list_init();
  host_list_init();
  job_list_init();
  load_foss_config(NULL);
  interface_init();
  database_init();
  load_agent_config();

  signal(SIGCHLD, chld_sig);
  signal(SIGALRM, prnt_sig);
  signal(SIGTERM, prnt_sig);
  signal(SIGQUIT, prnt_sig);
  signal(SIGINT,  prnt_sig);
  signal(SIGHUP,  prnt_sig);

  /* *********************************** */
  /* *** post initialization checks **** */
  /* *********************************** */
  if(db_reset)
    database_reset_queue();
  if(test_die)
    closing = 1;

  /* *************************************** */
  /* *** enter the scheduler event loop **** */
  /* *************************************** */
  event_signal(database_update_event, NULL);
  alarm(CHECK_TIME);
  event_loop_enter(update_scheduler);

  return close_scheduler();
}
