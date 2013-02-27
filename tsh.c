/* 
 * tsh - A tiny shell program with job control
 * 
 * <Charlie Severson>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

pid_t Fork(void);
void Kill(pid_t pid, int sig);
int Sigemptyset(sigset_t *set);
int Sigaddset(sigset_t *set, int signum); 
int Sigprocmask(int SIG, sigset_t *set,sigset_t * rewrite); 


/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
 
/******************
 * Mask Section 
*******************/ 

pid_t Fork(void)
{
	pid_t pid;
	if ((pid = fork()) < 0)
		{
		unix_error("Fork error");
		}
	return pid;
}
								/* Wrapper for Kill */ 
void Kill(pid_t pid, int signum)
{	
	int signal;
								/* Allows PID not found error (nothing happens) */ 
	if(( signal = kill(pid,signum)) < 0 && errno != ESRCH) 
		{
		unix_error("Kill error");
		}
} 

								/* All errors for the blocking functions are allowed */ 
int Sigemptyset(sigset_t *set)
{
	int signal; 
	if(( signal = sigemptyset(set)) < 0) 
		{
		unix_error("Sigemptyset error");
		}
	return signal; 
}

int Sigaddset(sigset_t *set, int signum)
{
	int signal; 
	if((signal = sigaddset(set, signum)) < 0) 
		{
		unix_error("Sigaddset error");
		}
	return signal; 
}

int Sigprocmask(int SIG, sigset_t *set, sigset_t * rewrite)
{
	int signal; 
	if(( signal = sigprocmask(SIG, set, rewrite)) < 0) 
		{
		unix_error("Sigpromask error");
		}
	return signal; 
} 
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
	char* argv[MAXARGS];         				/* Array that will hold command line arguments */ 
	char buf[MAXLINE];					/* Holds modified command line */
	int bg; 	                   			/* Boolean for telling if command is bg or fg */           
	sigset_t mask;                	 			/* Used to create the blocking set */ 
	pid_t pid;                   				/* Process id */
		
	strcpy(buf, cmdline);
	bg = parseline(cmdline, argv);    			/* Parse the command line */ 

								/* Return right away if nothing is on the command line */
	if(argv[0] == NULL)      
		return; 					/* Ignore empty lines */
								/* Check to see if the command is built-in.  Run it, if so.  */ 
	if (!builtin_cmd(argv)) 
		{
								/* Set up for blocking SIGCHLD */ 
		Sigemptyset(&mask);
		Sigaddset(&mask, SIGCHLD); 
		Sigprocmask(SIG_BLOCK, &mask, NULL); 		/* Block SIGCHLD */
								
								/* As job list is edited, start processing child signals */
		if((pid = Fork()) == 0) 			/* Child runs user job */
			{  
								/* Inside child */ 
			Sigprocmask(SIG_UNBLOCK, &mask, NULL);	/* Unblock SIGCHLD in new process */ 
			setpgid(0,0);                  		/* Put child in a new process group */ 
								/* Execute command */ 
			if(execve(argv[0], argv, environ) < 0) 
				{	
				printf("%s: Command not found. \n", argv[0]); 
				exit(0); 
				}
			}	
								/* Inside shell / parent */ 
								/* Parent waits for foreground job to terminate */
								/* If fg job */ 
		if(!bg)                              
			{
			if(addjob(jobs, pid, FG, cmdline)) 
				{				/* Add job to shell data */
				Sigprocmask(SIG_UNBLOCK, &mask, NULL);  /* Unblock SIGCHLD */
				waitfg(pid);			/* Wait on fg process */ 
				}
			}					/* If bg job */ 
		else
			{
			if(addjob(jobs, pid, BG, cmdline))
				{			 	/* Add job to shell data */
				Sigprocmask(SIG_UNBLOCK, &mask, NULL);
				printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); 
								/* Don't wait this time, so print out info */ 							}
			}
		}
	
    return;   
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
	static char array[MAXLINE]; 				/* holds command line local copy */
	char *buf = array;        				/* ptr that traverses command line */
	char *delim;              				/* points to delimiters (white space) */
	int argc;                 				/* number of args */
	int bg;                   				/* bg job? */

	strcpy(buf, cmdline);					/* strcopy copies the C string pointed by source into the array 								 * pointed by destination, including the terminating null 
								 * character (and stopping at that point) */
	buf[strlen(buf)-1] = ' ';  				/* replace trailing '\n' with space */
	while (*buf && (*buf == ' ')) 				/* ignore leading spaces */
		buf++;

    								/* Build the argv list */
  	argc = 0;
  	if (*buf == '\'')
		{
		buf++;
		delim = strchr(buf, '\'');			/* strchr returns a pointer to the first occurrence of character 									 * in the C string str */
	    	}
	else 
		{
		delim = strchr(buf, ' ');
		}

	while (delim) 
		{
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf && (*buf == ' ')) 	
								/* Ignore spaces */
		       	buf++;
		if (*buf == '\'') 
			{
		    	buf++;
		   	delim = strchr(buf, '\'');
			}
		else 
			{
			delim = strchr(buf, ' ');
			}
	    	}
	argv[argc] = NULL;
	    
	if (argc == 0)						/* Ignore blank line */				
	    	{			
		return 1;
		}
    								/* Should the job run in the background? */
    if ((bg = (*argv[argc - 1] == '&')) != 0) 
		{
		argv[--argc] = NULL;
   		}
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
								/* If argv[0] is quit, exit the shell */ 
	if(!strcmp(argv[0], "quit"))				/* strcmp compares the C string str1 to the C string str2 */
		{
		exit(0);          
		}          
	else if((!strcmp(argv[0], "bg")) || (!strcmp(argv[0], "fg")))
								/* If arv[0] is "bg" or "fg", do: */
		{ 
		do_bgfg(argv);     				/* Call fucntion that executes those commands */
		return 1;
		}
	else if(!strcmp(argv[0], "jobs"))    			/* If arv[0] is "jobs", do: */ 
		{
		listjobs(jobs); 				/* Call fucntion that executes those commands */		
		return 1;
		}
	else
		{						/* Not a builtin command */
		return 0;
		}    	
} 

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{

	int jobid;              				/* Pointers get information about process */ 
	pid_t pidt; 	
	struct job_t *jid; 
	int is_BG = (!strcmp(argv[0], "bg"));  			/* True if argv is a bg process */ 	
	
	if(is_BG)
								/* Error checking */  
		{
		if(!argv[1]) 					/* Bg job */			
			{
			printf("bg command requires PID or %%jobid argument \n");
			return; 
			}					/* atoi parses the C string str interpreting its content as an 
								 * integral number, which is returned as an int value. */
		if((atoi(argv[1]+1) == 0 && argv[1][0] == '%') || (atoi(argv[1]) == 0 && argv[1][0] != '%')) 
			{
			printf("bg: argument must be a PID or %%jobid \n");
			return;
			}
		}
	else
		{ 
		if(!argv[1]) 					/* Fg job */
			{
			printf("fg command requires PID or %%jobid argument \n");
			return; 
			}
		if((atoi(argv[1]+1) == 0 && argv[1][0] == '%') || (atoi(argv[1]) == 0 && argv[1][0] != '%')) 
			{
			printf("fg: argument must be a PID or %%jobid \n");
			return;
			}
		}			
	if(argv[1][0] == '%')
								/* If either command is denoting process by Job ID */ 
		{
		jobid = atoi(argv[1]+1);      
								/* Get job ID from command line */ 
		if((jid = getjobjid(jobs, jobid)) == NULL)
								/* Get job struct of job ID */  
			{
			printf("%%%d: No such job \n", jobid);
			return; 
			}
		pidt = jid->pid;  
								/* Get process ID of that job */ 
		}
								/* Similar idea for command using pid */ 
	else 
		{
		pidt = atoi(argv[1]);
			if((jid = getjobpid(jobs, pidt)) == NULL)
			{
			printf("(%d): No such process \n", pidt); 
			return;
			}
		jobid = jid->pid; 
		}  
	 
								/* If command is bg */ 
	if(is_BG) 
		{ 
		jid->state = BG;      				/* Change state to bg */ 
		Kill(-pidt, SIGCONT);  				/* Reset and continue command */ 
		printf("[%d] (%d) %s", jobid, pidt, jid->cmdline);   /* Print out info */ 
		}
								/* Similar idea for fg, but now we must wait since it is now in 								 * the fg */ 
	else 
		{
		jid->state = FG; 				/* If command is fg */
		Kill(-pidt, SIGCONT);				/* Change state to fg */ 
		waitfg(pidt);		
		}
 	return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
	struct job_t *jid;
	jid = getjobpid(jobs, pid);      			/* Get job struct from PID */ 

								/* Run a loop while there is still a fg process 
								 * and the fg process is not in ST joblist state */
	while(fgpid(jobs) && jid->state != ST) 			
		{
		sleep(1);
		} 
	if(verbose)
		{
		printf("waitfg: process (%d) is no longer the foreground process \n", pid); 
		}
  return;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
	if(verbose)
		{ 
		printf("sigchld_handler: entering \n"); 
		pid_t pid; 
		int status, jobid; 
		}
								/* While there are un-reaped children:
		 						 * WNOHANG: Don't block waiting
		 						 * WUNTRACED: Report status of stopped children */ 
	while((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0)
  		{
		jobid = pid2jid(pid);      			/* Get the job ID from the PID */
								/* If the child is stopped */ 
		if(WIFSTOPPED(status)) 				/* Returns true if the child that caused the return is stopped */
			{
			getjobpid(jobs, pid)->state = ST;  	/* Adjust the state of that job to stopped */ 
			printf("Job [%d] (%d) stopped by signal %d \n", jobid, pid, WSTOPSIG(status));
			}
								/* If the process was terminated by a signal */ 
		if(WIFSIGNALED(status)) 			/* Return true if the child process terminated because of a
								 * signal that was not caught */
			{
								/* Delete the job */ 
			deletejob(jobs,pid); 
			if(verbose) 
				printf("sigchld_handler: Job [%d] (%d) deleted \n", jobid, pid );
				printf("Job [%d] (%d) terminated by signal %d \n", jobid, pid, WTERMSIG(status));
			}
		if(WIFEXITED(status))				/* Returns true if child terminates normally */ 		
			{
			deletejob(jobs,pid); 
			if(verbose) 
				{
				printf("sigchld_handler: Job [%d] (%d) deleted \n", jobid, pid );
				printf("sigchld_handler: Job [%d] (%d) terminated okay (status 0) \n", jobid, pid );
				}
			}
		}
								/* Allow ECHILD and EINTR errors */ 
								/* i.e., if the calling process has not children (ECHILD),
								 * or waitpid was interrupted (EINTR) */
	if(ECHILD != errno && EINTR != errno) 
		{
		unix_error("waitpid error");
		} 
	if(verbose) 
		{
		printf("sigchld_handler: exiting \n"); 	
		}				
	return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
								/* Search jobs for fg process */
	if(verbose)
		{ 
		printf("sigint_handler: entering \n");
		}
	pid_t pid = fgpid(jobs);
	int jobid = pid2jid(pid);
								/* Send SIGINT to fg porcess. */ 
	 							/* Negative PID kills the entire process group */
	Kill(-pid, sig);
	if(verbose)
		{ 
		printf("sigint_handler: Job [%d] (%d) killed \n",jobid, pid);
		}
	if(verbose)
		{ 
		printf("sigint_handler: exiting \n");
		}
  	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
								/* Search jobs for fg process */
	if(verbose) 
		{
		printf("sigtstp_handler: entering \n");
		}
	pid_t pid = fgpid(jobs);
	int jobid = pid2jid(pid);
								/* Send SIGINT to fg porcess. */ 
	 							/* Negative PID kills the entire process group */
	Kill(-pid, SIGTSTP);   
	if(verbose) 
		{
		printf("sigtstp_handler: Job [%d] (%d) stopped \n",jobid, pid);
		}
	if(verbose) 
		{
		printf("sigtstp_handler: exiting \n");
		}
  	return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



