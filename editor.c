#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <asm-generic/errno-base.h>
#include <asm-generic/ioctls.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define editor_version "0.0.1"
#define TAB_LENGTH 4

enum keys {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  PG_UP,
  PG_DN,
  HOME,
  END,
  DEL
};

struct erow {
  int size;
  int rsize;
  char *line;
  char *render;
};

struct editor {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int rows;
  int cols;
  int numrows;
  struct erow *row;
  char *filename;
  char status[80];
  time_t statusmsg_time;
  struct termios og;
};

struct editor E;

void kill(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disable_raw() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.og) == -1) {
    kill("tcsetattr");
  }
}

void rawmode() {
  if (tcgetattr(STDIN_FILENO, &E.og) == -1) {
    kill("tcgetattr");
  }
  atexit(disable_raw);
  struct termios raw = E.og;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= ~(BRKINT | IXON | ICRNL | INPCK | ISTRIP);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag &= (CS8);

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    kill("tcsetattr");
  }
}

int readkey() {
  int n;
  char c;
  while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
    if (n == -1 && errno == EAGAIN)
      kill("read");
  }

  if (c == '\x1b') {
    char sq[3];

    if (read(STDIN_FILENO, &sq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &sq[1], 1) != 1)
      return '\x1b';

    if (sq[0] == '[') {
      if (sq[1] >= '0' && sq[1] <= '9') {
        if (read(STDIN_FILENO, &sq[2], 1) != 1)
          return '\x1b';
        if (sq[2] == '~') {
          switch (sq[1]) {
          case '1':
            return HOME;
          case '3':
            return DEL;
          case '4':
            return END;
          case '5':
            return PG_UP;
          case '6':
            return PG_DN;
          case '7':
            return HOME;
          case '8':
            return END;
          }
        }
      } else {
        switch (sq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME;
        case 'F':
          return END;
        }
      }
    } else if (sq[0] == '0') {
      switch (sq[1]) {
      case 'H':
        return HOME;
      case 'F':
        return END;
      }
    }
    return '\x1b';
  } else {
    return c;
  }
}

int cursorposition(int *rows, int *cols) {
  char buf[32];
  long unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    ++i;
  }
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;
  return 0;
}

int windowsize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return cursorposition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

int cxtorx(struct erow *row, int cx) {
  int rx = 0;
  for (int i = 0; i < cx; i++) {
    if (row->line[i] == '\t')
      rx += (TAB_LENGTH - 1) - (rx % TAB_LENGTH);
    rx++;
  }
  return rx;
}

void updaterow(struct erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->line[j] == '\t')
      tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs * (TAB_LENGTH - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->line[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % TAB_LENGTH != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->line[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAddRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(struct erow) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].line = malloc(len + 1);
  memcpy(E.row[at].line, s, len);
  E.row[at].line[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  updaterow(&E.row[at]);
  E.numrows++;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  FILE *fp = fopen(filename, "r");
  if (!fp)
    kill("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorAddRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAdd(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

void scroll() {
  E.rx = 0;
  if (E.cy < E.numrows)
    E.rx = cxtorx(&E.row[E.cy], E.cx);
  if (E.cy < E.rowoff)
    E.rowoff = E.cy;
  if (E.cy >= E.rowoff + E.rows) {
    E.rowoff = E.cy - E.rows + 1;
  }
  if (E.rx < E.coloff)
    E.coloff = E.rx;
  if (E.rx >= E.coloff + E.cols)
    E.coloff = E.rx - E.cols + 1;
}

void drawrows(struct abuf *ab) {
  for (int y = 0; y < E.rows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (y == E.rows / 3 && E.numrows == 0) {
        char message[80];
        int messagelen =
            snprintf(message, sizeof(message), "Batata editor -- version %s",
                     editor_version);
        if (messagelen > E.cols)
          messagelen = E.cols;

        int padding = (E.cols - messagelen) / 2;
        if (padding < 0)
          padding = 0;
        if (padding) {
          abAdd(ab, "~", 1);
          padding--;
        }
        while (padding--)
          abAdd(ab, " ", 1);
        abAdd(ab, message, messagelen);
      } else {
        abAdd(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.cols)
        len = E.cols;
      abAdd(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAdd(ab, "\x1b[K", 3);
    abAdd(ab, "\r\n", 2);
  }
}

void DrawStatusBar(struct abuf *ab) {
  abAdd(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                     E.filename ? E.filename : "[No file]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
  if (len > E.cols)
    len = E.cols;
  abAdd(ab, status, len);
  while (len < E.cols) {
    if (E.cols - len == rlen) {
      abAdd(ab, rstatus, rlen);
      break;
    } else {
      abAdd(ab, " ", 1);
      len++;
    }
  }
  abAdd(ab, "\x1b[m", 3);
  abAdd(ab, "\r\n", 2);
}

void DrawMessageBar(struct abuf *ab) {
  abAdd(ab, "\x1b[K", 3);
  int msglen = strlen(E.status);
  if (msglen > E.cols)
    msglen = E.cols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAdd(ab, E.status, msglen);
}

void setstatus(const char *format, ...) {
  va_list arglist;
  va_start(arglist, format);
  vsnprintf(E.status, sizeof(E.status), format, arglist);
  va_end(arglist);
  E.statusmsg_time = time(NULL);
}

void clearscreen() {
  scroll();
  struct abuf ab = ABUF_INIT;

  abAdd(&ab, "\x1b[?25l", 6);
  abAdd(&ab, "\x1b[H", 3);
  abAdd(&ab, "\x1b[2J", 4);

  drawrows(&ab);
  DrawStatusBar(&ab);
  DrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
           E.rx - E.coloff + 1);
  abAdd(&ab, buf, strlen(buf));

  abAdd(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void movecursor(int key) {
  struct erow *row = (E.cy >= E.rows) ? NULL : &E.row[E.cy];
  switch (key) {
  case ARROW_LEFT:
    if (E.cx != 0)
      E.cx--;
    else if (E.cy > 0) {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;
  case ARROW_DOWN:
    if (E.cy < E.numrows)
      E.cy++;
    break;
  case ARROW_UP:
    if (E.cy != 0)
      E.cy--;
    break;
  case ARROW_RIGHT:
    if (row && E.cx < row->size)
      E.cx++;
    else if (row && E.cx == row->size) {
      E.cy++;
      E.cx = 0;
    }
    break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen)
    E.cx = rowlen;
}

void processkey() {
  int c = readkey();
  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2j", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case PG_UP:
  case PG_DN: {
    if (c == PG_UP)
      E.cy = E.rowoff;
    else if (c == PG_DN) {
      E.cy = E.rowoff + E.rows - 1;
      if (E.cy > E.numrows)
        E.cy = E.numrows;
    }

    int times = E.rows;
    while (times--)
      movecursor(c == PG_UP ? ARROW_UP : ARROW_DOWN);
  } break;

  case HOME:
    E.cx = 0;
    break;

  case END: {
    if (E.cy < E.numrows)
      E.cx = E.row[E.cy].size;
  } break;

  case ARROW_LEFT:
  case ARROW_DOWN:
  case ARROW_UP:
  case ARROW_RIGHT:
    movecursor(c);
    break;
  }
}

void geteditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;
  E.status[0] = '\0';
  E.statusmsg_time = 0;
  if (windowsize(&E.rows, &E.cols) == -1)
    kill("GetWindowSize");
  E.rows -= 2;
}

int main(int argc, char *argv[]) {
  rawmode();
  geteditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  setstatus("TIP: Ctrl-Q to quit");

  while (1) {
    clearscreen();
    processkey();
  }

  return 0;
}
