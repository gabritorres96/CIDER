/* Writes its (Windows) process id as 4 raw bytes to stdout: shows the guest runs in the worker, not in ORVYN. */
extern int getpid(void);
extern long write(int fd, const void *buf, unsigned long n);
int main(void) {
    int pid = getpid();
    write(1, &pid, 4);
    return 0;
}
