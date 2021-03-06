/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include<ctype.h>
#include<errno.h>
#include<fcntl.h>
#include<stdarg.h>
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/ioctl.h>
#include<sys/types.h>
#include<termios.h>
#include<time.h>
#include<unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.3"

#define ENABLE_LINE_NUM (1 << 0)
#define ENABLE_AUTO_INDENT (1 << 1)

#define CTRL_KEY(k) ((k) & 0x1f)  //a bit mask that sets bits 5 and 6 bits of the character to 0, which is exactly how CTRL works
enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editor_highlight {
    HL_NORMAL,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

typedef int bool;
/*** data ***/

struct editor_syntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;  //raw text
    char *render;  //rendered text
    unsigned char *hl;
    bool hl_open_comment;
}erow;

struct editor_config {
    int cx, cy;   //cursor position in the chars field, starting at 0
    int rx;       //cursor position in the render field, starting at 0
    int rowoff;   //row offset for vertical scrolling
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    int row_num_offset;
    erow *row;
    int dirty;
    int quit_times;
    char *filename;
    char statusmsg[80];
    uint8_t options;
    time_t statusmsg_time;
    struct editor_syntax *syntax;
    struct termios orig_terminos;

    int KILO_TAB_STOP;
    int KILO_QUIT_TIMES;
};

struct editor_config E;

/*** filetypes ***/

char  *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};
char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};

struct editor_syntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))
/*** prototypes ***/

void editor_set_status_message(const char *fmt, ...);
void editor_move_cursor(int key);
void editor_refresh_screen();
char *editor_prompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    //perror() looks at the global errno variable and prints a descriptive error message for it. 
    //It also prints the string given to it before it prints the error message, 
    //which is meant to provide context about what part of your code caused the error.
    perror(s);
    exit(1);
}

void disable_raw_mode()
{
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_terminos) == -1)
        die("tcsetattr");
}

void enable_raw_mode()
{
    if(tcgetattr(STDIN_FILENO, &E.orig_terminos) == -1) die("tcgetattr");
    atexit(disable_raw_mode); //register the function to be called when program exits

    struct termios raw = E.orig_terminos;
    //switch off a couple flags by making the bit on the corresponding position 0
    //iflag, oflag, cflag, lflag are unsigned int
    //BRKINT: switch off break condition which terminates the program
    //ICRNL: switch off carriage returns being translated into newlines
    //INPCK: switch off parity checking (most likely obsolete)
    //ISTRIP: disable the 8th bit of each input byte being set to 0
    //IXON: switch off software flow control
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    //OPOST: switch off \n being translated into \r\n
    raw.c_oflag &= ~(OPOST);

    //CS8: set the character size to 8 bits per byte
    raw.c_cflag &= (CS8);

    //ECHO: stop printing the typed word in the terminal
    //ICANON: switch from line-by-line input to letter-by-letter
    //IEXTEN: switch off literal input (such as Ctrl-C being intputted as 3)
    //ISIG: switch off Ctr-C(terminate) and Ctr-Z(suspend) signal
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    //c_cc an array of unsigned char, size 32
    //VMIN: the minimum number of input bytes needed before read() can return
    //VMAX: the maximum amount of time to wait before read() returns, unit: 0.1s.
    //If read() times out, it returns 0.
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) ==-1) die("tcsetattr");
}

int editor_read_key() 
{
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if(nread == -1 && errno != EAGAIN) die("read");
    }

    if(c == '\x1b') {
        char seq[3];

        //if read() times out, will return <esc>
        if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if(seq[0] == '[') {
            if(seq[1] >= '0' && seq[1] <= '9') {
                if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if(seq[2] == '~') {
                    switch(seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch(seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if(seq[0] == '0') {
            switch(seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int get_cursor_position(int *rows, int *cols)
{
    char buf[32];
    unsigned int i=0;
    //"6n" inquires the cursor position
    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while(i < sizeof(buf) -1) {
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[i] == 'R') break;
        i++;
    }
    buf[i]='\0';

    if(buf[0] != '\x1b' || buf[1] != '[') return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int get_window_size(int *rows, int *cols)
{
    struct winsize ws;

    //'C' moves the cursor to the right
    //'B' moves the cursor down. Both commands check the bound
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        editor_read_key();
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** init ***/

int get_int(char *p) {
    int ret = 0;
    while(*p && *p != '\r' && *p != '\n') {
        ret = ret * 10 + *p - '0';
        p++;
    }
    return ret;
}

void read_config_file() {
    FILE *fp = fopen(".kilorc", "r");
    if(!fp) return;

    char *line = NULL;
    char *p;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        if(strstr(line, "LineNumbers") != NULL) {
            p = line + strlen("LineNumbers") + 1;
            if(*p == '1') {
                E.options |= ENABLE_LINE_NUM;
            }
        } else if(strstr(line, "AutoIndent") != NULL) {
            p = line + strlen("AutoIndent") + 1;
            if(*p == '1') {
                E.options |= ENABLE_AUTO_INDENT;
            }
        } else if(strstr(line, "TabStop") != NULL) {
            p = line + strlen("TabStop") + 1;
            E.KILO_TAB_STOP = get_int(p);
        } else if(strstr(line, "QuitTimes") != NULL) {
            p = line + strlen("QuitTimes") + 1;
            E.KILO_QUIT_TIMES = get_int(p);
        }
    }
}

void init_editor() 
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;  //scroll to the top of the file by default
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename =  NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;
    E.row_num_offset = 0;
    E.options = 0;
    if(get_window_size(&E.screenrows, &E.screencols) == -1) die("get_window_size");
    E.screenrows -= 2;
    E.KILO_QUIT_TIMES = 3;
    E.KILO_TAB_STOP = 8;
    read_config_file();
    E.quit_times = E.KILO_QUIT_TIMES;
}

/*** sytax highlighting ***/

int is_separator(int c) 
{
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editor_update_syntax(erow *row)
{
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if(E.syntax == NULL) return;

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;
    
    bool prev_sep = 1;
    bool in_string = 0;
    bool in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while(i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        //check for singleline comment
        //when in multiline comment, singleline comment shouldn't be recognized
        if(scs_len && !in_string && !in_comment) {
            if(!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        //check for multiline comment
        if(mcs_len && mce_len && !in_string) {
            if(in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if(!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if(!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }
        //check for strings
        if(E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if(in_string) {  //in_string is either ' or "
                row->hl[i] = HL_STRING;
                if(c == '\\' && i + 1 < row->size) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if(c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if(c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        //check for numbers
        if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        //check for keywords
        if(prev_sep) {
            int j;
            for(j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if(kw2) klen--;

                if(!strncmp(&row->render[i], keywords[j], klen) &&
                   is_separator(row->render[i + klen])) {
                       memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                       i += klen;
                       break;
                   }
            }
            if(keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }
        prev_sep = is_separator(c);
        i++;
    }

    //since a user could comment out the entire file by changing one line, we should check if lines below need to be rerendered recursively
    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if(changed && row->idx + 1 < E.numrows)
        editor_update_syntax(&E.row[row->idx + 1]);
}

int editor_syntax_to_color(int hl)
{
    switch(hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;
        case HL_KEYWORD1: return 33;
        case HL_KEYWORD2: return 32;
        case HL_STRING: return 35;
        case HL_NUMBER: return 31;
        case HL_MATCH: return 34;
        default: return 37;
    }
}

void editor_select_syntax_highlight()
{
    E.syntax = NULL;
    if(E.filename == NULL) return;

    char *ext = strrchr(E.filename, '.');

    for(unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editor_syntax *s = &HLDB[j];
        unsigned int i = 0;
        while(s->filematch[i]) {
            int is_ext = s->filematch[i][0] == '.';
            if((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
               (!is_ext && strstr(E.filename, s->filematch[i]))) {
                   E.syntax = s;

                   int filerow;
                   for(filerow = 0; filerow < E.numrows; filerow++) {
                       editor_update_syntax(&E.row[filerow]);
                   }
                   return;
               }
            i++;
        }
    }
}
/*** row operations ***/

int deciLength(int num) {
    int ret = 0;
    if(!num) return 1;
    do {
        ++ret;
    } while(num /= 10);
    return ret;
}

int editor_row_cx_to_rx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for(j = 0; j < cx; j++) {
        if(row->chars[j] == '\t')
            rx += (E.KILO_TAB_STOP - 1) - (rx % E.KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

int editor_row_rx_to_cx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++) {
        if(row->chars[cx] == '\t')
            cur_rx += (E.KILO_TAB_STOP - 1) - (cur_rx % E.KILO_TAB_STOP);
        cur_rx++;
        if(cur_rx > rx) return cx;
    }
    return cx;
}

void editor_update_row(erow *row)
{
    //update rsize and render field
    int tabs = 0;
    int j = 0;
    for(j = 0; j < row->size; j++)
        if(row->chars[j] == '\t') tabs++;
    
    free(row->render); //it's Ok to free a NULL pointer
    row->render = malloc(row->size + tabs * (E.KILO_TAB_STOP - 1) + 1); //row->size already counts 1 for each tab

    int idx = 0;
    for(j = 0; j <row->size; j++) {
        if(row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while(idx % E.KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else 
            row->render[idx++] = row->chars[j];
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editor_update_syntax(row);
}

void editor_update_row_offset() {
    E.screencols += (E.row_num_offset + 1);
    E.row_num_offset = deciLength(E.numrows);
    E.screencols -= (E.row_num_offset + 1);
}

void editor_insert_row(int at, char *s, size_t len, int leading_sps)
{
    if(at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for(int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

    E.row[at].idx= at;
    E.row[at].size = len + leading_sps;
    E.row[at].chars = malloc(len + leading_sps + 1);
    for(int i = 0; i < leading_sps; ++i) {
        E.row[at].chars[i] = ' ';
    }
    memcpy(E.row[at].chars + leading_sps, s, len);
    E.row[at].chars[len + leading_sps] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editor_update_row(&E.row[at]);
    E.numrows++;
    if(E.options & ENABLE_LINE_NUM) {
        editor_update_row_offset();
    }
    E.dirty++;
}

void editor_free_row(erow *row)
{
    free(row->chars);
    free(row->render);
    free(row->hl);
}

void editor_del_row(int at)
{
    if(at < 0 || at >= E.numrows) return;
    editor_free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for(int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c)
{
    if(at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2); //one for the new character, one for \0
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    E.dirty++;
}

void editor_row_del_char(erow *row, int at)
{
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

/*** editor operations ***/

void editor_insert_char(int c)
{
    if(E.cy == E.numrows) {
        editor_insert_row(E.numrows, "", 0, 0);
    }
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editor_del_char()
{
    //this function is like backspace
    if(E.cy == E.numrows) return;
    if(E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if(E.cx > 0) {
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editor_row_append_string(&E.row[E.cy - 1], row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}

int get_leading_sps(int line) {
    int i;
    for(i = 0; E.row[line].render[i] == ' '; ++i);
    return i;
}

void editor_insert_new_line() // create a new line when typeing enter
{
    int leading_sps = 0;
    if(E.options & ENABLE_AUTO_INDENT) {
        leading_sps = get_leading_sps(E.cy);
    }
    if(E.cx == 0) {
        editor_insert_row(E.cy, "", 0, leading_sps);
    } else {
        erow *row = &E.row[E.cy];
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx, leading_sps);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    E.cy++;
    E.cx = leading_sps;
}

/*** file i/o ***/

char *editor_rows_to_string(int *buflen) 
{
    int totlen = 0;
    int j;
    for(j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;  // /r/n is stripped off when reading from file
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p= buf;
    for(j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_open(char *filename) 
{
    free(E.filename);
    E.filename = strdup(filename);

    editor_select_syntax_highlight();

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    //size_t: unsigned int(32)/unsigned long(64)
    //ssize_t: int(32)/long (64)
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        //strip off return carriage
        while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_insert_row(E.numrows, line, linelen, 0); // no auto indent needed
    }
    if(E.options & ENABLE_LINE_NUM) {
        editor_update_row_offset();
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save() 
{
    if(E.filename == NULL) {
        E.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if(E.filename == NULL) {
            editor_set_status_message("Save aborted");
            return;
        }
        editor_select_syntax_highlight();
    }

    int len;
    char *buf = editor_rows_to_string(&len);
    
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1) {
        if(ftruncate(fd, len) != -1)  //set file size to specific length
            if(write(fd, buf, len) == len) {
                editor_set_status_message("%d bytes written to disk", len);
                E.dirty = 0;
                close(fd);
                free(buf);
                return;
            }
        close(fd);
    }
    
    free(buf);
    editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editor_find_callback(char *query, int key)
{
    static int y_to_start = 0;
    static int x_to_start = 0;
    static int direction = 1;

    int len = strlen(query);
    int switch_direction = 0;

    if(key == '\r' || key == '\x1b' || !len) {
        y_to_start = E.cy;
        x_to_start = E.cx;
        direction = 1;
        return;
    } else if(key == ARROW_RIGHT || key == ARROW_DOWN) {
        if(direction == -1)
            switch_direction = 1;
        direction = 1;
    } else if(key == ARROW_LEFT || key == ARROW_UP) {
        if(direction == 1)
            switch_direction = 1;
        direction = -1;
    } else {
        y_to_start = E.cy;
        x_to_start = E.cx;
        direction = 1;
    }

    if(y_to_start == 0) {
        direction = 1;
    }

    static int saved_hl_line;
    static char* saved_hl = NULL;

    if(saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }
    int current_y = y_to_start;
    int lines_visited = 0;
    while(lines_visited < E.numrows) {
        erow *row = &E.row[current_y];
        char *match = NULL;
        if(direction == 1) {
            if(switch_direction) {
                x_to_start += len << 1;
                switch_direction = 0;
            }
            if(x_to_start >= row->rsize || !(match = strstr(row->render + x_to_start, query))) {
                lines_visited++;
                current_y += direction;
                if(current_y == -1) current_y = E.numrows - 1;
                else if(current_y == E.numrows) current_y = 0;
                x_to_start = 0;
                continue;
            }
        } else {
            if(switch_direction) {
                x_to_start -= len << 1;
                switch_direction = 0;
            }
            while(x_to_start >= 0 && strncmp(row->render + x_to_start, query, len))
                x_to_start--;
            if(x_to_start < 0) {
                lines_visited++;
                current_y += direction;
                if(current_y == -1) current_y = E.numrows - 1;
                else if(current_y == E.numrows) current_y = 0;
                x_to_start = E.row[current_y].rsize - len;
                continue;
            }
        }

        y_to_start = current_y;
        x_to_start = direction == 1 ? match - row->render + len : x_to_start - len;
        E.cy = current_y;
        E.cx = direction == 1 ? editor_row_rx_to_cx(row, match - row->render) : editor_row_rx_to_cx(row, x_to_start + len);

        saved_hl_line = current_y;
        saved_hl = malloc(row->rsize);
        memcpy(saved_hl, row->hl, row->rsize);
        memset(&row->hl[direction == 1 ? match - row->render : x_to_start + len], HL_MATCH, len);
        return;
    }
}

void editor_find()
{
    char *query = editor_prompt("Search: %s (ESC/Arrows/Enter)", editor_find_callback);
    
    if(query)
        free(query);
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}  //constructor for abuf type

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output ***/

void editor_scroll()
{
    E.rx = 0;
    if(E.cy < E.numrows)
        E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);

    if(E.cy < E.rowoff) //scroll upwards
        E.rowoff = E.cy; 
    if(E.cy >= E.rowoff + E.screenrows) //scroll downwards
        E.rowoff = E.cy - E.screenrows + 1;  //cursor is at the bottom
    if(E.rx < E.coloff)
        E.coloff = E.rx;
    if(E.rx >= E.coloff + E.screencols)
        E.coloff = E.rx - E.screencols + 1;    
}

void editor_draw_rows(struct abuf *ab)
{
    int y;
    for(y = 0; y < E.screenrows; y++) {
        //file row points to the line in the file
        //while y points to the line on the screen
        int filerow = y + E.rowoff;

        if(filerow >= E.numrows) {
            if(E.numrows == 0 && y == E.screenrows / 3) { // the row for the welcome message
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
                if(welcomelen > E.screencols) welcomelen = E.screencols;
                // center the welcome message
                int padding = (E.screencols - welcomelen) / 2;
                if(padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else
                abAppend(ab, "~" , 1);
        } else {
            if(E.options & ENABLE_LINE_NUM) {
                char linenum[32];
                snprintf(linenum, sizeof(linenum), "%*d ", E.row_num_offset, filerow + 1);
                abAppend(ab, linenum, strlen(linenum));
            }

            int len = E.row[filerow].rsize - E.coloff;
            if(len < 0) len = 0;
            if(len > E.screencols) len = E.screencols;

            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            int j;
            for(j = 0; j < len; j++) {
                if(iscntrl(c[j])) {
                    char sym = (c[j] <= 26 ? '@' + c[j] : '?');
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    //x1b[m turns off all text formatting, so need to recolor the text
                    if(current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if(hl[j] == HL_NORMAL) {
                    if(current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);         
                } else {
                    int color = editor_syntax_to_color(hl[j]);
                    if(color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }


            abAppend(ab, "\x1b[39m", 5);
        }
        //clear each line as we redraw them instead of clearing the entire page
        abAppend(ab, "\x1b[K", 3); 
        abAppend(ab, "\r\n", 2);
         
    }
}

void editor_draw_status_bar(struct abuf *ab)
{
    //m causes the text printed after it t be printed with various attributes
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No name]", E.numrows, E.dirty ? "(modified)" : "");
    if(len > E.screencols) len = E.screencols;
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no ft",E.cy + 1, E.numrows);
    abAppend(ab, status, len); 
    while(len < E.screencols + E.row_num_offset) {
        if(E.screencols + E.row_num_offset - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab,"\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screencols) msglen = E.screencols;
    if(msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editor_refresh_screen()
{
    editor_scroll();

    struct abuf ab = ABUF_INIT; //use buffer so that we only need to do one write(), avoiding flickering

    //"?25l" hides the cursor when refreshing the screen to prevent flickering
    abAppend(&ab, "\x1b[?25l", 6);

    //'H' positions the cursor. It takes two arguments, specifying the line and column, say "/x1b[1;1H"
    //Line and column number start at 1
    abAppend(&ab, "\x1b[H", 3);
    
    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    bool line_num_enabled = E.options & ENABLE_LINE_NUM;
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff + E.row_num_offset + (line_num_enabled != 0 ? 1 : 0)) + 1);
    abAppend(&ab, buf, strlen(buf));

    //"?25h" shows the cursor after refreshing
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editor_set_status_message(const char *fmt, ...)
{
    //va_list is a pointer that points to the variable parameters
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editor_prompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0]= '\0';

    while(1) {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();
        int c = editor_read_key();
        
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if(buflen !=0 ) buf[--buflen] = '\0';
        } else if(c == '\x1b') {
            editor_set_status_message("");
            if(callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if(c == '\r') {
            if(buflen != 0) {
                editor_set_status_message("");
                if(callback) callback(buf, c);
                return buf;
            }
        } else if(!iscntrl(c) && c < 128) {
            if(buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if(callback) callback(buf, c);
    }
}

void editor_move_cursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch(key) {
        case ARROW_LEFT:
            if(E.cx != 0)
                E.cx--;
            else if(E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if(row && E.cx < row->size)
                E.cx++;
            else if(row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows) { //E.cy == E.numrows - 1 -> reach the end of the file
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if(E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editor_process_keypress() 
{
    int c = editor_read_key();

    switch(c) {
        case '\r':
            editor_insert_new_line();
            break;

        case CTRL_KEY('q'):
            if(E.dirty && E.quit_times > 0) {
                editor_set_status_message("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.", E.quit_times);
                E.quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
        break;

        case CTRL_KEY('s'):
            editor_save();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if(E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        case CTRL_KEY('f'):
            editor_find();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY) editor_move_cursor(ARROW_RIGHT);
            editor_del_char();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            { //the braces are for declaration of variables
                if(c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if(E.cy > E.numrows) E.cy = E.numrows;
                }
                int times = E.screenrows;
                while(times--)
                    editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editor_move_cursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;
        
        default:
            editor_insert_char(c);
            break;
    }
}

int main(int argc, char *argv[]) 
{
    enable_raw_mode();
    init_editor();
    if(argc >= 2)
        editor_open(argv[1]);

    editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while(1) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
