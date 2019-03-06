/* tar.c - create/extract archives
 *
 * Copyright 2014 Ashwini Kumar <ak.ashwini81@gmail.com>
 *
 * For the command, see
 *   http://pubs.opengroup.org/onlinepubs/007908799/xcu/tar.html
 * For the modern file format, see
 *   http://pubs.opengroup.org/onlinepubs/9699919799/utilities/pax.html#tag_20_92_13_06
 *   https://en.wikipedia.org/wiki/Tar_(computing)#File_format
 *   https://www.gnu.org/software/tar/manual/html_node/Tar-Internals.html
 *
 * Toybox will never implement the "pax" command as a matter of policy.
 *
 * For writing to external program
 * http://www.gnu.org/software/tar/manual/html_node/Writing-to-an-External-Program.html
 *
 * Why --exclude pattern but no --include? tar cvzf a.tgz dir --include '*.txt'
 * Extract into dir same as filename, --restrict? "Tarball is splodey"
 *

USE_TAR(NEWTOY(tar, "&(no-recursion)(numeric-owner)(no-same-permissions)(overwrite)(exclude)*(to-command):o(no-same-owner)p(same-permissions)k(keep-old)c(create)|h(dereference)x(extract)|t(list)|v(verbose)j(bzip2)z(gzip)O(to-stdout)m(touch)X(exclude-from)*T(files-from)*C(directory):f(file):[!txc][!jz]", TOYFLAG_USR|TOYFLAG_BIN))

todo: support .txz
todo: directory timestamps not set on extract
todo: extract into chmod 000 directory

config TAR
  bool "tar"
  default n
  help
    usage: tar [-cxtjzhmvO] [-X FILE] [-T FILE] [-f TARFILE] [-C DIR]

    Create, extract, or list files in a .tar (or compressed t?z) file. 

    Options:
    c  Create                x  Extract               t  Test
    f  Name of TARFILE       C  Change to DIR first   v  Verbose: show filenames
    o  Ignore owner          h  Follow symlinks       m  Ignore mtime
    j  Force bzip2 format    z  Force gzip format
    O  Extract to stdout
    X  File of names to exclude
    T  File of names to include
    --exclude=FILE File pattern(s) to exclude
*/

#define FOR_tar
#include "toys.h"

GLOBALS(
  char *f, *C;
  struct arg_list *T, *X;
  char *to_command;
  struct arg_list *exc;

// exc is an argument but inc isn't?
  struct arg_list *inc, *pass;
  void *inodes, *handle;
  char *cwd;
  int fd;
  unsigned short offset; // only ever used to calculate 512 byte padding
)

struct tar_hdr {
  char name[100], mode[8], uid[8], gid[8],size[12], mtime[12], chksum[8],
       type, link[100], magic[8], uname[32], gname[32], major[8], minor[8],
       prefix[155], padd[12];
};

// Parsed information about a tar header.
struct file_header {
  char *name, *link_target, *uname, *gname;
  off_t size;
  uid_t uid;
  gid_t gid;
  mode_t mode;
  time_t mtime;
  dev_t device;
};

struct archive_handler {
  struct file_header file_hdr;
  void (*extract_handler)(struct archive_handler*);
};

struct inode_list {
  struct inode_list *next;
  char *arg;
  ino_t ino;
  dev_t dev;
};

static void copy_in_out(int src, int dst, off_t size)
{
  int i, rd, rem = size%512, cnt;
  
  cnt = size/512 + (rem?1:0);

  for (i = 0; i < cnt; i++) {
    rd = (i == cnt-1 && rem) ? rem : 512;
    xreadall(src, toybuf, rd);
    writeall(dst, toybuf, rd);
  }
}

//convert to octal
static void itoo(char *str, int len, off_t val)
{
  char *t, tmp[sizeof(off_t)*3+1];
  int cnt = sprintf(tmp, "%0*llo", len, (unsigned long long)val);

  t = tmp + cnt - len;
  if (*t == '0') t++;
  memcpy(str, t, len);
}

// This really needs a hash table
static struct inode_list *seen_inode(void **list, struct stat *st, char *name)
{
  if (!S_ISDIR(st->st_mode) && st->st_nlink > 1) {
    struct inode_list *new;

    for (new = *list; new; new = new->next)
      if(new->ino == st->st_ino && new->dev == st->st_dev)
        return new;

    new = xzalloc(sizeof(*new));
    new->ino = st->st_ino;
    new->dev = st->st_dev;
    new->arg = xstrdup(name);
    new->next = *list;
    *list = new;
  }
  return 0;
}

static void write_longname(struct archive_handler *tar, char *name, char type)
{
  struct tar_hdr tmp[2];
  unsigned int sum = 0;
  int i, sz = strlen(name) +1;

  memset(tmp, 0, sizeof(tmp));
  strcpy(tmp->name, "././@LongLink");
  memset(tmp->mode, '0', sizeof(tmp->mode)-1);
  memset(tmp->uid, '0', sizeof(tmp->uid)-1);
  memset(tmp->gid, '0', sizeof(tmp->gid)-1);
  itoo(tmp->size, sizeof(tmp->size), sz);
  memset(tmp->mtime, '0', sizeof(tmp->mtime)-1);
  tmp->type = type;
  strcpy(tmp->magic, "ustar  ");

  // Calculate checksum
  memset(tmp->chksum, ' ', 8);
  for (i = 0; i < 512; i++) sum += ((char *)tmp)[i];
// TODO: why is this -1 when the rest aren't?
  itoo(tmp->chksum, sizeof(tmp->chksum)-1, sum);

  // write header and name, padded with NUL to block size
  xwrite(TT.fd, tmp, sizeof(*tmp));
  xwrite(TT.fd, name, sz);
  xwrite(TT.fd, tmp+1, 512-(sz%512));
}

static int filter(struct arg_list *lst, char *name)
{
  struct arg_list *cur;

  for (cur = lst; cur; cur = cur->next)
    if (!fnmatch(cur->arg, name, 1<<3)) return 1;
  return 0;
}

static void add_file(struct archive_handler *tar, char **nam, struct stat *st)
{
  struct tar_hdr hdr;
  struct passwd *pw;
  struct group *gr;
  struct inode_list *node;
  int i, fd =-1;
  char *c, *p, *name = *nam, *lnk, *hname, buf[512] = {0,};
  unsigned int sum = 0;
  static int warn = 1;

  for (p = name; *p; p++)
    if ((p == name || p[-1] == '/') && *p != '/' && filter(TT.exc, p)) return;

  if (S_ISDIR(st->st_mode) && name[strlen(name)-1] != '/') {
    lnk = xmprintf("%s/",name);
    free(name);
    *nam = name = lnk;
  }
  hname = name;
  //remove leading '/' or relative path '../' component
  if (*hname == '/') hname++;
  if (!*hname) return;
  while ((c = strstr(hname, "../"))) hname = c + 3;
  if (warn && hname != name) {
    fprintf(stderr, "removing leading '%.*s' from member names\n",
           (int)(hname-name), name);
    warn = 0;
  }

  memset(&hdr, 0, sizeof(hdr));
  strncpy(hdr.name, hname, sizeof(hdr.name));
  itoo(hdr.mode, sizeof(hdr.mode), st->st_mode &07777);
  itoo(hdr.uid, sizeof(hdr.uid), st->st_uid);
  itoo(hdr.gid, sizeof(hdr.gid), st->st_gid);
  itoo(hdr.size, sizeof(hdr.size), 0); //set size later
  itoo(hdr.mtime, sizeof(hdr.mtime), st->st_mtime);
  memset(hdr.chksum, ' ', sizeof(hdr.chksum));

  // Hard link or symlink?
  i = !!S_ISLNK(st->st_mode);
  if (i || (node = seen_inode(&TT.inodes, st, hname))) {
// TODO: test preserve symlink ownership
    hdr.type = '1'+i;
    if (!(lnk = i?xreadlink(name):node->arg)) return perror_msg("readlink");
// TODO: does this need NUL terminator?
    if (strlen(lnk) > sizeof(hdr.link))
      write_longname(tar, lnk, 'K'); //write longname LINK
// TODO: this will error_exit() if too long, not truncate.
    xstrncpy(hdr.link, lnk, sizeof(hdr.link));
    if (i) free(lnk);
  } else if (S_ISREG(st->st_mode)) {
    hdr.type = '0';
    if (st->st_size <= (off_t)077777777777LL)
      itoo(hdr.size, sizeof(hdr.size), st->st_size);
    else {
// TODO: test accept 12 7's but don't emit without terminator
      return error_msg("TODO: need base-256 encoding for '%s' '%lld'\n",
                hname, (unsigned long long)st->st_size);
    }
  } else if (S_ISDIR(st->st_mode)) hdr.type = '5';
  else if (S_ISFIFO(st->st_mode)) hdr.type = '6';
  else if (S_ISBLK(st->st_mode) || S_ISCHR(st->st_mode)) {
    hdr.type = (S_ISCHR(st->st_mode))?'3':'4';
    itoo(hdr.major, sizeof(hdr.major), dev_major(st->st_rdev));
    itoo(hdr.minor, sizeof(hdr.minor), dev_minor(st->st_rdev));
  } else return error_msg("unknown file type '%o'", st->st_mode & S_IFMT);

  if (strlen(hname) > sizeof(hdr.name))
          write_longname(tar, hname, 'L'); //write longname NAME
  strcpy(hdr.magic, "ustar  ");
  if ((pw = getpwuid(st->st_uid)))
    snprintf(hdr.uname, sizeof(hdr.uname), "%s", pw->pw_name);
  else snprintf(hdr.uname, sizeof(hdr.uname), "%d", st->st_uid);

  if ((gr = getgrgid(st->st_gid)))
    snprintf(hdr.gname, sizeof(hdr.gname), "%s", gr->gr_name);
  else snprintf(hdr.gname, sizeof(hdr.gname), "%d", st->st_gid);

  //calculate chksum.
  for (i = 0; i < 512; i++) sum += ((char*)&hdr)[i];
  itoo(hdr.chksum, sizeof(hdr.chksum)-1, sum);
  if (FLAG(v)) printf("%s\n",hname);
  xwrite(TT.fd, (void*)&hdr, 512);

  //write actual data to archive
  if (hdr.type != '0') return; //nothing to write
  if ((fd = open(name, O_RDONLY)) < 0) {
    perror_msg("can't open '%s'", name);
    return;
  }
  copy_in_out(fd, TT.fd, st->st_size);
  if (st->st_size%512) writeall(TT.fd, buf, (512-(st->st_size%512)));
  close(fd);
}

static int add_to_tar(struct dirtree *node)
{
  struct stat st;
  char *path;
  struct archive_handler *hdl = (struct archive_handler*)TT.handle;

  if (!dirtree_notdotdot(node)) return 0;
  if (!fstat(TT.fd, &st) && st.st_dev == node->st.st_dev
      && st.st_ino == node->st.st_ino) {
    error_msg("'%s' file is the archive; not dumped", TT.f);
    return 0;
  }

  path = dirtree_path(node, 0);
  add_file(hdl, &path, &(node->st)); //path may be modified
  free(path);
  if (FLAG(no_recursion)) return 0;
  return ((DIRTREE_RECURSE | (FLAG(h)?DIRTREE_SYMFOLLOW:0)));
}

static void compress_stream(struct archive_handler *tar_hdl)
{
  int pipefd[2];
  pid_t cpid;

  xpipe(pipefd);

  signal(SIGPIPE, SIG_IGN);
  cpid = fork();
  if (cpid == -1) perror_exit("fork");

  if (!cpid) {    /* Child reads from pipe */
    char *argv[] = {FLAG(z)?"gzip":"bzip2", "-f", NULL};
    xclose(pipefd[1]); /* Close unused write*/
    dup2(pipefd[0], 0);
    dup2(TT.fd, 1); //write to tar fd
    xexec(argv);
  } else {
    xclose(pipefd[0]);          /* Close unused read end */
// TODO doesn't this leak pipefd[1]?
    dup2(pipefd[1], TT.fd); //write to pipe
  }
}

static void extract_to_stdout(struct archive_handler *tar)
{
  struct file_header *file_hdr = &tar->file_hdr;

  copy_in_out(TT.fd, 0, file_hdr->size);
  TT.offset += file_hdr->size;
}

static void extract_to_command(struct archive_handler *tar)
{
  int pipefd[2], status = 0;
  pid_t cpid;
  struct file_header *file_hdr = &tar->file_hdr;

  xpipe(pipefd);
  if (!S_ISREG(file_hdr->mode)) return; //only regular files are supported.

  cpid = fork();
  if (cpid == -1) perror_exit("fork");

  if (!cpid) {    // Child reads from pipe
    char buf[64], *argv[4] = {"sh", "-c", TT.to_command, NULL};

    setenv("TAR_FILETYPE", "f", 1);
    sprintf(buf, "%0o", file_hdr->mode);
    setenv("TAR_MODE", buf, 1);
    sprintf(buf, "%ld", (long)file_hdr->size);
    setenv("TAR_SIZE", buf, 1);
    setenv("TAR_FILENAME", file_hdr->name, 1);
    setenv("TAR_UNAME", file_hdr->uname, 1);
    setenv("TAR_GNAME", file_hdr->gname, 1);
    sprintf(buf, "%0o", (int)file_hdr->mtime);
    setenv("TAR_MTIME", buf, 1);
    sprintf(buf, "%0o", file_hdr->uid);
    setenv("TAR_UID", buf, 1);
    sprintf(buf, "%0o", file_hdr->gid);
    setenv("TAR_GID", buf, 1);

    xclose(pipefd[1]); // Close unused write
    dup2(pipefd[0], 0);
    signal(SIGPIPE, SIG_DFL);
    xexec(argv);
  } else {
    xclose(pipefd[0]);  // Close unused read end
    copy_in_out(TT.fd, pipefd[1], file_hdr->size);
    TT.offset += file_hdr->size;
    xclose(pipefd[1]);
    waitpid(cpid, &status, 0);
    if (WIFSIGNALED(status))
      xprintf("tar : %d: child returned %d\n", cpid, WTERMSIG(status));
  }
}

static void extract_to_disk(struct archive_handler *tar)
{
  int flags, dst_fd = -1;
  char *s;
  struct stat ex;
  struct file_header *file_hdr = &tar->file_hdr;

// while not if
  flags = strlen(file_hdr->name);
  if (flags>2)
    if (strstr(file_hdr->name, "/../") || !strcmp(file_hdr->name, "../") ||
        !strcmp(file_hdr->name+flags-3, "/.."))
      error_msg("drop %s", file_hdr->name);

  if (file_hdr->name[flags-1] == '/') file_hdr->name[flags-1] = 0;
  //Regular file with preceding path
  if ((s = strrchr(file_hdr->name, '/'))) {
    if (mkpath(file_hdr->name) && errno !=EEXIST) {
      error_msg(":%s: not created", file_hdr->name);
      return;
    }
  }

  //remove old file, if exists
  if (!FLAG(k) && !S_ISDIR(file_hdr->mode) && !lstat(file_hdr->name, &ex))
    if (unlink(file_hdr->name))
      perror_msg("can't remove: %s", file_hdr->name);

  //hard link
  if (S_ISREG(file_hdr->mode) && file_hdr->link_target) {
    if (link(file_hdr->link_target, file_hdr->name))
      perror_msg("can't link '%s' -> '%s'",file_hdr->name, file_hdr->link_target);
    goto COPY;
  }

  switch (file_hdr->mode & S_IFMT) {
    case S_IFREG:
      flags = O_WRONLY|O_CREAT|O_EXCL;
      if (FLAG(overwrite)) flags = O_WRONLY|O_CREAT|O_TRUNC;
      dst_fd = open(file_hdr->name, flags, file_hdr->mode & 07777);
      if (dst_fd == -1) perror_msg("%s: can't open", file_hdr->name);
      break;
    case S_IFDIR:
      if ((mkdir(file_hdr->name, file_hdr->mode) == -1) && errno != EEXIST)
        perror_msg("%s: can't create", file_hdr->name);
      break;
    case S_IFLNK:
      if (symlink(file_hdr->link_target, file_hdr->name))
        perror_msg("can't link '%s' -> '%s'",file_hdr->name, file_hdr->link_target);
      break;
    case S_IFBLK:
    case S_IFCHR:
    case S_IFIFO:
      if (mknod(file_hdr->name, file_hdr->mode, file_hdr->device))
        perror_msg("can't create '%s'", file_hdr->name);
      break;
    default:
      printf("type not yet supported\n");
      break;
  }

  //copy file....
COPY:
  copy_in_out(TT.fd, dst_fd, file_hdr->size);
  TT.offset += file_hdr->size;
  close(dst_fd);

  if (S_ISLNK(file_hdr->mode)) return;
  if (!FLAG(o)) {
    //set ownership..., --no-same-owner, --numeric-owner
    uid_t u = file_hdr->uid;
    gid_t g = file_hdr->gid;

    if (!FLAG(numeric_owner)) {
      struct group *gr = getgrnam(file_hdr->gname);
      struct passwd *pw = getpwnam(file_hdr->uname);
      if (pw) u = pw->pw_uid;
      if (gr) g = gr->gr_gid;
    }
    if (!geteuid() && chown(file_hdr->name, u, g))
      perror_msg("chown %d:%d '%s'", u, g, file_hdr->name);;
  }

  if (FLAG(p)) // || !FLAG(no_same_permissions))
    chmod(file_hdr->name, file_hdr->mode);

  //apply mtime
  if (!FLAG(m)) {
    struct timeval times[2] = {{file_hdr->mtime, 0},{file_hdr->mtime, 0}};
    utimes(file_hdr->name, times);
  }
}

static void add_to_list(struct arg_list **llist, char *name)
{
  struct arg_list **list = llist;

  while (*list) list=&((*list)->next);
  *list = xzalloc(sizeof(struct arg_list));
  (*list)->arg = name;
  if ((name[strlen(name)-1] == '/') && strlen(name) != 1)
    name[strlen(name)-1] = '\0';
}

static void file_to_list(char *file, struct arg_list **llist)
{
  int fd = xopenro(file);
  char *line = 0;

  while ((line = get_line(fd))) add_to_list(llist, xstrdup(line));
  if (fd) close(fd);
  free(line);
}

//convert octal to int
static int otoi(char *str, int len)
{
  long val;
  char *endp, inp[len+1]; //1 for NUL termination

  memcpy(inp, str, len);
  inp[len] = '\0'; //nul-termination made sure
  val = strtol(inp, &endp, 8);
  if (*endp && *endp != ' ') error_exit("invalid param");
  return (int)val;
}

static void extract_stream(struct archive_handler *tar_hdl)
{
  int pipefd[2];              
  pid_t cpid;                 

  xpipe(pipefd);

  cpid = fork();
  if (cpid == -1) perror_exit("fork");

  if (!cpid) {    /* Child reads from pipe */
    char *argv[] =
      {FLAG(z)?"gunzip":"bunzip2", "-cf", "-", NULL};
    xclose(pipefd[0]); /* Close unused read*/
    dup2(TT.fd, 0);
    dup2(pipefd[1], 1); //write to pipe
    xexec(argv);
  } else {
    xclose(pipefd[1]);          /* Close unused read end */
    dup2(pipefd[0], TT.fd); //read from pipe
  }
}

static char *process_extended_hdr(struct archive_handler *tar, int size)
{
  char *value = NULL, *p, *buf = xzalloc(size+1);

  if (readall(TT.fd, buf, size) != size) error_exit("short read");
  buf[size] = 0;
  TT.offset += size;
  p = buf;

  while (size) {
    char *key;
    int len, n;

    // extended records are of the format: "LEN NAME=VALUE\n"
    sscanf(p, "%d %n", &len, &n);
    key = p + n;
    p += len;
    size -= len;
    p[-1] = 0;
    if (size < 0) {
      error_msg("corrupted extended header");
      break;
    }

    len = strlen("path=");
    if (!strncmp(key, "path=", len)) {
      value = key + strlen("path=");
      break;
    }
  }
  if (value) value = xstrdup(value);
  free(buf);
  return value;
}

static void unpack_tar(struct archive_handler *tar_hdl)
{
  struct tar_hdr tar;
  struct file_header *file_hdr;
  int i, j, maj, min, sz, e = 0;
  unsigned int cksum;
  char *longname = NULL, *longlink = NULL;

  while (1) {
    cksum = 0;
    if (TT.offset % 512) {
      sz = 512 - TT.offset % 512;
      lskip(TT.fd, sz);
      TT.offset += sz;
    }
    i = readall(TT.fd, &tar, 512);
    TT.offset += i;
    if (i != 512) {
      if (i >= 2) goto CHECK_MAGIC; //may be a small (<512 byte)zipped file
      error_exit("read error");
    }

    if (!tar.name[0]) {
      if (e) return; //end of tar 2 empty blocks
      e = 1;//empty jump to next block
      continue;
    }
    if (strncmp(tar.magic, "ustar", 5)) {
      // Try detecting .gz or .bz2 by looking for their magic.
CHECK_MAGIC:
      if ((!strncmp(tar.name, "\x1f\x8b", 2) || !strncmp(tar.name, "BZh", 3))
          && !lseek(TT.fd, -i, SEEK_CUR)) {
        toys.optflags |= (*tar.name == 'B') ? FLAG_j : FLAG_z;
        TT.offset -= i;
        extract_stream(tar_hdl);
        continue;
      }
      error_exit("invalid tar format");
    }

    for (j = 0; j<148; j++) cksum += (unsigned int)((char*)&tar)[j];
    for (j = 156; j<500; j++) cksum += (unsigned int)((char*)&tar)[j];
    //cksum field itself treated as ' '
    for ( j= 0; j<8; j++) cksum += (unsigned int)' ';

    if (cksum != otoi(tar.chksum, sizeof(tar.chksum))) error_exit("wrong cksum");

    file_hdr = &tar_hdl->file_hdr;
    memset(file_hdr, 0, sizeof(struct file_header));
    file_hdr->mode = otoi(tar.mode, sizeof(tar.mode));
    file_hdr->uid = otoi(tar.uid, sizeof(tar.uid));
    file_hdr->gid = otoi(tar.gid, sizeof(tar.gid));
    file_hdr->size = otoi(tar.size, sizeof(tar.size));
    file_hdr->mtime = otoi(tar.mtime, sizeof(tar.mtime));
    file_hdr->uname = xstrdup(tar.uname);
    file_hdr->gname = xstrdup(tar.gname);
    maj = otoi(tar.major, sizeof(tar.major));
    min = otoi(tar.minor, sizeof(tar.minor));
    file_hdr->device = dev_makedev(maj, min);

    if (tar.type <= '7') {
      if (tar.link[0]) {
        sz = sizeof(tar.link);
        file_hdr->link_target = xmalloc(sz + 1);
        memcpy(file_hdr->link_target, tar.link, sz);
        file_hdr->link_target[sz] = '\0';
      }

      file_hdr->name = xzalloc(256);// pathname supported size
      if (tar.prefix[0]) {
        memcpy(file_hdr->name, tar.prefix, sizeof(tar.prefix));
        sz = strlen(file_hdr->name);
        if (file_hdr->name[sz-1] != '/') file_hdr->name[sz] = '/';
      }
      sz = strlen(file_hdr->name);
      memcpy(file_hdr->name + sz, tar.name, sizeof(tar.name));
      if (file_hdr->name[255]) error_exit("filename too long");
    }

    switch (tar.type) {
      //    case '\0':
      case '0':
      case '7':
      case '1': //Hard Link
        file_hdr->mode |= S_IFREG;
        break;
      case '2':
        file_hdr->mode |= S_IFLNK;
        break;
      case '3':
        file_hdr->mode |= S_IFCHR;
        break;
      case '4':
        file_hdr->mode |= S_IFBLK;
        break;
      case '5':
        file_hdr->mode |= S_IFDIR;
        break;
      case '6':
        file_hdr->mode |= S_IFIFO;
        break;
      case 'K':
        longlink = xzalloc(file_hdr->size +1);
        xread(TT.fd, longlink, file_hdr->size);
        TT.offset += file_hdr->size;
        continue;
      case 'L':
        free(longname);
        longname = xzalloc(file_hdr->size +1);           
        xread(TT.fd, longname, file_hdr->size);
        TT.offset += file_hdr->size;
        continue;
      case 'D':
      case 'M':
      case 'N':
      case 'S':
      case 'V':
      case 'g':  // pax global header
        lskip(TT.fd, file_hdr->size);
        TT.offset += file_hdr->size;
        continue;
      case 'x':  // pax extended header
        free(longname);
        longname = process_extended_hdr(tar_hdl, file_hdr->size);
        continue;
      default: break;
    }

    if (longname) {
      free(file_hdr->name);
      file_hdr->name = longname;
      longname = NULL;
    }
    if (longlink) {
      free(file_hdr->link_target);
      file_hdr->link_target = longlink;
      longlink = NULL;
    }

    if ((file_hdr->mode & S_IFREG) && 
        file_hdr->name[strlen(file_hdr->name)-1] == '/') {
      file_hdr->name[strlen(file_hdr->name)-1] = '\0';
      file_hdr->mode &= ~S_IFREG;
      file_hdr->mode |= S_IFDIR;
    }

    if ((file_hdr->link_target && *(file_hdr->link_target)) 
        || S_ISLNK(file_hdr->mode) || S_ISDIR(file_hdr->mode))
      file_hdr->size = 0;

    if (filter(TT.exc, file_hdr->name) ||
        (TT.inc && !filter(TT.inc, file_hdr->name))) goto SKIP;
    add_to_list(&TT.pass, xstrdup(file_hdr->name));

    if (FLAG(t)) {
      if (FLAG(v)) {
        char perm[11];
        struct tm *lc = localtime((const time_t*)&(file_hdr->mtime));

        mode_to_string(file_hdr->mode, perm);
        printf("%s %s/%s %9ld %d-%02d-%02d %02d:%02d:%02d ",perm,file_hdr->uname,
            file_hdr->gname, (long)file_hdr->size, 1900+lc->tm_year,
            1+lc->tm_mon, lc->tm_mday, lc->tm_hour, lc->tm_min, lc->tm_sec);
      }
      printf("%s",file_hdr->name);
      if (file_hdr->link_target) printf(" -> %s",file_hdr->link_target);
      xputc('\n');
SKIP:
      lskip(TT.fd, file_hdr->size);
      TT.offset += file_hdr->size;
    } else {
      if (FLAG(v)) printf("%s\n",file_hdr->name);
      tar_hdl->extract_handler(tar_hdl);
    }
    free(file_hdr->name);
    free(file_hdr->link_target);
    free(file_hdr->uname);
    free(file_hdr->gname);
  }
}

void tar_main(void)
{
  struct archive_handler *tar_hdl;
  struct arg_list *tmp;
  char *s, **args = toys.optargs;

  if (!geteuid()) toys.optflags |= FLAG_p;

  // Collect file list
  while (*args) add_to_list(&TT.inc, *args++);
  for (;TT.T; TT.T = TT.T->next) file_to_list(TT.T->arg, &TT.inc);
  for (;TT.X; TT.X = TT.X->next) file_to_list(TT.X->arg, &TT.exc);

  // Open archive file
  if (FLAG(c)) {
    if (!TT.inc) error_exit("empty archive");
    TT.fd = 1;
  }
  if (TT.f && strcmp(TT.f, "-"))
    TT.fd = xcreate(TT.f, TT.fd*(O_WRONLY|O_CREAT|O_TRUNC), 0666);

  // Get destination directory
  if (TT.C) xchdir(TT.C);
  TT.cwd = xabspath(s = xgetcwd(), 1);
  free(s);

// TODO: zap
  tar_hdl = xzalloc(sizeof(struct archive_handler));
  tar_hdl->extract_handler = extract_to_disk;

  // Are we reading?
  if (FLAG(x)||FLAG(t)) {
    if (FLAG(O)) tar_hdl->extract_handler = extract_to_stdout;
    if (FLAG(to_command)) {
      signal(SIGPIPE, SIG_IGN);
      tar_hdl->extract_handler = extract_to_command;
    }
// no j?
// TODO: autodtect
    if (FLAG(z)) extract_stream(tar_hdl);
    unpack_tar(tar_hdl);
    for (tmp = TT.inc; tmp; tmp = tmp->next)
      if (!filter(TT.exc, tmp->arg) && !filter(TT.pass, tmp->arg))
        error_msg("'%s' not in archive", tmp->arg);

  // are we writing? (Don't have to test flag here one of 3 must be set)
  } else {
// TODO: autodetect
    if (FLAG(j)||FLAG(z)) compress_stream(tar_hdl);

    for (tmp = TT.inc; tmp; tmp = tmp->next) {
      TT.handle = tar_hdl;
      //recurse thru dir and add files to archive
      dirtree_flagread(tmp->arg, FLAG(h)?DIRTREE_SYMFOLLOW:0, add_to_tar);
    }
    memset(toybuf, 0, 1024);
    writeall(TT.fd, toybuf, 1024);
  }
}
