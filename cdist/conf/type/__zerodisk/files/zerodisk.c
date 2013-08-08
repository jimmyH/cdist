/*
 * zerodisk - Write zeroes to any unused space on a partition
 *
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <math.h>
#include <syslog.h>
#include <string.h>
#include <signal.h>

int verbose = 1;
int fd = 0;
char* filename = NULL;

void sig_handler(int sig)
{
  if (sig==SIGINT || sig==SIGTERM)
  {
    syslog(LOG_DAEMON|LOG_ERR,"Caught signal %d",sig);
    if (fd>0) close(fd);
    if (filename) syslog(LOG_DAEMON|LOG_ERR,"Warning not deleting %s",filename);
    _exit(1);
  }
}

void usage(char* c)
{
  printf("usage: zerodisk -f <filename> --min <%%> --max <%%> --freq <seconds> --rate <kB/s>\n");
  printf("  -f <filename>    - Name of file full of zeroes\n");
  printf("  --min <%%>  - Minimum amount of free disk space before reducing the zero file\n");
  printf("  --max <%%>  - Maximum amount of free disk space before increasing the zero file\n");
  printf("  --freq <seconds> - How often to monitor the amount of free disk space\n");
  printf("  --rate <kB/s> -    Max IO rate to write zeroes\n");
  exit(0);
}

unsigned long get_blocksize(int fd)
{
  struct statvfs buf;
  if (fstatvfs(fd,&buf)==-1)
  {
    char errbuf[1024];
    strerror_r(errno,errbuf,sizeof(errbuf));
    syslog(LOG_DAEMON|LOG_ERR,"Failed to stat filesystem: %s",errbuf);
    close(fd);
    exit(1);
  }
  return buf.f_bsize;
}

double get_diskfree(int fd)
{
  struct statvfs buf;
  if (fstatvfs(fd,&buf)==-1)
  {
    char errbuf[1024];
    strerror_r(errno,errbuf,sizeof(errbuf));
    syslog(LOG_DAEMON|LOG_ERR,"Failed to stat filesystem: %s",errbuf);
    close(fd);
    exit(1);
  }
  return ( (double)100.0 * buf.f_bavail ) / buf.f_blocks; // Note this is based on unprivileged users free space.
}

uint64_t get_disksize(int fd)
{
  struct statvfs buf;
  if (fstatvfs(fd,&buf)==-1)
  {
    char errbuf[1024];
    strerror_r(errno,errbuf,sizeof(errbuf));
    syslog(LOG_DAEMON|LOG_ERR,"Failed to stat filesystem: %s",errbuf);
    close(fd);
    exit(1);
  }
  return (uint64_t) (buf.f_blocks)*buf.f_frsize;
}

uint64_t get_filesize(int fd)
{
  struct stat buf;
  if (fstat(fd,&buf)==-1)
  {
    char errbuf[1024];
    strerror_r(errno,errbuf,sizeof(errbuf));
    syslog(LOG_DAEMON|LOG_ERR,"Failed to stat zerofile: %s",errbuf);
    close(fd);
    exit(1);
  }
  return (uint64_t)buf.st_size;
}


int main(int argc,char** argv)
{
  int min = 0;
  int max = 0;
  int ratelimit = 0;
  int freq = 0;
  int i = 1;
  struct flock lck;

  if (argc!=11) usage(argv[0]);

  while (i<argc)
  {
    if (0==strcmp(argv[i],"-f")) { ++i; if (i<argc) filename=argv[i]; }
    else if (0==strcmp(argv[i],"--min")) { ++i; if (i<argc) min=atoi(argv[i]); }
    else if (0==strcmp(argv[i],"--max")) { ++i; if (i<argc) max=atoi(argv[i]); }
    else if (0==strcmp(argv[i],"--freq")) { ++i; if (i<argc) freq=atoi(argv[i]); }
    else if (0==strcmp(argv[i],"--rate")) { ++i; if (i<argc) ratelimit=atoi(argv[i]); }
    ++i;
  }

  if (!filename || !min || !max || !ratelimit || !freq) usage(argv[0]);

  if (signal(SIGINT,sig_handler)==SIG_ERR || signal(SIGTERM,sig_handler)==SIG_ERR)
  {
    perror("Failed to setup signal handler");
    exit(1);
  }

  fd = open(filename,O_RDWR | O_CREAT,S_IRWXU);
  if (fd==-1)
  {
    perror("Failed to open zerofile");
    exit(1);
  }

  daemon(1,0);

  // Try to lock the zerofile (must be done after calling daemon() because
  // locks are not inherited by children).
  lck.l_type = F_WRLCK;
  lck.l_whence = SEEK_SET;
  lck.l_start = 0;
  lck.l_len = 0;
  lck.l_pid = getpid();
  if (-1==fcntl(fd,F_SETLK,&lck))
  {
    char errbuf[1024];
    strerror_r(errno,errbuf,sizeof(errbuf));
    syslog(LOG_DAEMON|LOG_ERR,"Failed to lock file: %s",errbuf);
    close(fd);
    exit(1);
  }

  openlog(argv[0], LOG_PID, LOG_DAEMON);

  syslog(LOG_DAEMON|LOG_NOTICE,"monitoring %s\n",filename);

  uint64_t blocksize = get_blocksize(fd);
  syslog(LOG_DAEMON|LOG_NOTICE,"Blocksize is %lld\n",blocksize);
  ratelimit = (freq*ratelimit*1024)/blocksize;

  uint64_t disksize = get_disksize(fd);
  syslog(LOG_DAEMON|LOG_NOTICE,"Disksize is %lld GB\n",disksize/((uint64_t)1024*1024*1024));

  char * zeroblock = malloc(blocksize);
  if (!zeroblock)
  {
    char errbuf[1024];
    strerror_r(errno,errbuf,sizeof(errbuf));
    syslog(LOG_DAEMON|LOG_ERR,"Failed to malloc zeroblock: %s",errbuf);
    close(fd);
    exit(1);
  }

  for (i=0;i<blocksize;++i) zeroblock[i]=0;

  while (1)
  {
    double diskfree = get_diskfree(fd);
    uint64_t filesize = get_filesize(fd);

    if ( diskfree > max )
    {
      uint64_t delta = ((diskfree-max)/100)*disksize;
      delta = (delta/blocksize)*blocksize; // Round to blocksize

      if ( (off_t)-1==lseek(fd,0,SEEK_END))
      {
        char errbuf[1024];
        strerror_r(errno,errbuf,sizeof(errbuf));
        syslog(LOG_DAEMON|LOG_ERR,"Failed to lseek zerofile: %s",errbuf);
        close(fd);
        exit(1);
      }

      int i;
      uint64_t blocks = delta/blocksize;
      blocks = (blocks>ratelimit)?ratelimit:blocks;

      if (verbose)
      {
        syslog(LOG_DAEMON|LOG_INFO,"Percentage free %f, file size is %lld kB\n",diskfree,filesize/1024);
        syslog(LOG_DAEMON|LOG_INFO,"growing zerofile by %lld KB (%lld KB)\n",delta/1024,blocks*blocksize/1024);
      }
      for (i=0;i<blocks;++i)
      {
        if ( blocksize!=write(fd,zeroblock,blocksize))
        {
          char errbuf[1024];
          strerror_r(errno,errbuf,sizeof(errbuf));
          syslog(LOG_DAEMON|LOG_ERR,"Failed to write zerofile: %s",errbuf);
          close(fd);
          exit(1);
        }
      }

    }
    else if ( diskfree < min )
    {
      uint64_t delta = ((min-diskfree)/100)*disksize;
      delta = (delta/blocksize)*blocksize; // Round to blocksize
      if (delta>filesize) delta=filesize;
      if (delta>0)
      {
        if (verbose)
        {
          syslog(LOG_DAEMON|LOG_INFO,"Percentage free %f, file size is %lld kB\n",diskfree,filesize/1024);
          syslog(LOG_DAEMON|LOG_INFO,"shrinking zerofile by %lld KB\n",delta/1024);
        }
        if (-1==ftruncate(fd, (off_t)(filesize-delta)))
        {               
          char errbuf[1024];
          strerror_r(errno,errbuf,sizeof(errbuf));
          syslog(LOG_DAEMON|LOG_ERR,"Failed to truncate zerofile: %s",errbuf);
          close(fd);
          exit(1);
        }
      }
    }
      
    sleep(freq);
  }

  return 0;
}
