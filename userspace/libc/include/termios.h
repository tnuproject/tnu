#ifndef TNU_TERMIOS_H
#define TNU_TERMIOS_H

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define ECHO   0000010
#define ICANON 0000002
#define ISIG   0000001
#define IEXTEN 0000100
#define IXON   0000200
#define ICRNL  0000400
#define OPOST  0000001

/* c_cc indices */
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VTIME  5
#define VMIN   6
#define VSTART 8
#define VSTOP  9
#define VSUSP  10
#define VEOL   11

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);

#endif
