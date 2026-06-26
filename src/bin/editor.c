/*
 * @file editor.c
 * @brief Vix 4.0 - A Modern C Editor for JexOS.
 */

#define pr_fmt(fmt) "[EDITOR] " fmt
#include "kernel/printk.h"
#include "kernel/vsnprintf.h"

#include "editor.h"
#include "terminal.h"
#include "fs.h"
#include "kheap.h"
#include "string.h"
#include "tcc.h"
#include "exec.h"
#include "task.h"
#include "shell.h"
#include <stddef.h>
#include <stdint.h>

/* Constants */

#define EDITOR_WIDTH        80
#define EDITOR_HEIGHT       25
#define VISIBLE_HEIGHT      (EDITOR_HEIGHT - 1)

#define MAX_FILE_SIZE       32768
#define MAX_FILENAME_LEN    63
#define TAB_SIZE            4

#define CLIPBOARD_SIZE      4096
#define SEARCH_TERM_MAX     63
#define STATUS_MSG_MAX      79

/* Editor sub-modes */
#define MODE_NORMAL         0
#define MODE_SEARCH         1
#define MODE_GOTO           2

/* Syntax Highlighting Colors */
#define COL_DEFAULT     0x07    /* Light grey on black */
#define COL_COMMENT     0x08    /* Dark grey on black */
#define COL_STRING      0x06    /* Brown on black */
#define COL_NUMBER      0x0E    /* Yellow on black */
#define COL_PAREN       0x06    /* Brown on black */
#define COL_PREPROC     0x0D    /* Magenta on black */
#define COL_SYSCALL     0x0A    /* Light green on black */
#define COL_KEYWORD     0x0B    /* Cyan on black */
#define COL_FUNCALL     0x0E    /* Yellow on black */
#define COL_STATUSBG    0x70    /* Black on lightgrey */
#define COL_STATUSTXT   0x7C    /* Red on lightgrey */
#define COL_DIRTY       0x74    /* Red text, grey bg */
#define COL_SAVED_OK    0x2F    /* Green on cyan */
#define COL_SAVED_FAIL  0x4F    /* White on red */
#define COL_GUTTER      0x03    /* Cyan on black */
#define COL_PROMPT_BG   0x1F    /* White on blue */
#define COL_HELP        0x70    /* Black on lightgrey */

/* Syntax states */
#define SYNTAX_NORMAL       0
#define SYNTAX_COMMENT      1       /* // line comment */
#define SYNTAX_STRING       2       /* "string" */
#define SYNTAX_BLOCK_COMM   3

/* Undo Snapshot */

typedef struct {
    char*   saved_buffer;
    int     saved_length;
    int     saved_cursor_x;
    int     saved_cursor_y;
    int     saved_scroll_y;
    int     has_data;
} UndoSnapshot;

/* Editor State */

typedef struct {
    /* Core buffer */
    char*   buffer;
    int     length;
    char    filename[MAX_FILENAME_LEN + 1];

    /* Cursor / view */
    int     cursor_x;
    int     cursor_x_target;    /* Remember column for Up/Down vertical nav */
    int     cursor_y;
    int     scroll_y;

    /* Flags */
    int     running;
    int     dirty;
    int     save_status;        /* 0=none, 1=OK, 2=fail */
    int     quit_confirm;

    /* Undo */
    UndoSnapshot    undo;

    /* Clipboard */
    char    clipboard[CLIPBOARD_SIZE];
    int     clipboard_len;

    /* Line number minimum width (auto-grows for >999 lines) */
#define LINE_NUM_MIN_WIDTH  4

/* Sub-mode (search / goto) */
    int     sub_mode;
    char    search_term[SEARCH_TERM_MAX + 1];
    int     search_pos;         /* cursor pos in search/goto prompt */

    /* Transient status message */
    char    status_msg[STATUS_MSG_MAX + 1];
} EditorState;

/* Global instance */

static EditorState* s = NULL;

/*
 * editor_running - Extern used by shell.c to route input to editor.
 * Mirrors s->running for external consumers.
 */
int editor_running = 0;

/* Pre-computed syntax state */

static uint8_t syntax_state[MAX_FILE_SIZE];

/* Forward declarations */

static void render_text(void);
static void save_undo_snapshot(void);
static int  get_buffer_pos(int cursor_x, int cursor_y);
static int  save_file_internal(void);

/* Helpers */

/*
 * memmove_local - Copy memory with overlapping regions.
 * Uses forward copy when dest < src, backward when dest > src.
 * Required because kheap.h doesn't provide it (only memcpy/memset).
 */
static void* memmove_local(void* dest, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static int is_digit(char c)
{
    return (c >= '0' && c <= '9');
}

static void clear_status_msg(void)
{
    s->status_msg[0] = '\0';
}

static void set_status_msg(const char* msg)
{
    int i;
    for (i = 0; msg[i] && i < STATUS_MSG_MAX; i++)
        s->status_msg[i] = msg[i];
    s->status_msg[i] = '\0';
}

/* Syntax Highlighting */

static void precompute_syntax(void)
{
    int in_comment = 0, in_string = 0, in_block_comm = 0;
    int max = s->length < MAX_FILE_SIZE ? s->length : MAX_FILE_SIZE;

    for (int i = 0; i < max; i++) {
        if (!in_comment && !in_string && !in_block_comm) {
            if (i + 1 < max && s->buffer[i] == '/' && s->buffer[i + 1] == '/')
                in_comment = 1;
            else if (i + 1 < max && s->buffer[i] == '/' && s->buffer[i + 1] == '*')
                in_block_comm = 1;
            else if (s->buffer[i] == '"')
                in_string = 1;
        } else if (in_comment) {
            if (s->buffer[i] == '\n')
                in_comment = 0;
        } else if (in_block_comm) {
            if (i + 1 < max && s->buffer[i] == '*' && s->buffer[i + 1] == '/') {
                in_block_comm = 0;
                i++;    /* skip '/' */
            }
        } else if (in_string) {
            if (s->buffer[i] == '"' && (i == 0 || s->buffer[i - 1] != '\\'))
                in_string = 0;
        }
        if (in_block_comm)
            syntax_state[i] = SYNTAX_BLOCK_COMM;
        else if (in_comment)
            syntax_state[i] = SYNTAX_COMMENT;
        else if (in_string)
            syntax_state[i] = SYNTAX_STRING;
        else
            syntax_state[i] = SYNTAX_NORMAL;
    }
}

static uint8_t char_color(int pos)
{
    if (pos < 0 || pos >= s->length)
        return COL_DEFAULT;

    /* Syntax state look-up */
    switch (syntax_state[pos]) {
    case SYNTAX_COMMENT:    return COL_COMMENT;
    case SYNTAX_BLOCK_COMM: return COL_COMMENT;
    case SYNTAX_STRING:     return COL_STRING;
    default: break;
    }

    char c = s->buffer[pos];

    /* Closing quote */
    if (c == '"')
        return COL_STRING;

    /* Numbers */
    if (is_digit(c) && (pos == 0 || !is_alnum(s->buffer[pos - 1])))
        return COL_NUMBER;

    /* Braces / brackets / parens */
    if (c == '(' || c == ')' || c == '{' || c == '}'
        || c == '[' || c == ']')
        return COL_PAREN;

    /* Preprocessor directive (# at column 0) */
    if (c == '#' && (pos == 0 || s->buffer[pos - 1] == '\n'))
        return COL_PREPROC;

    if (!is_alnum(c))
        return COL_DEFAULT;

    /* Check word start */
    if (pos > 0 && is_alnum(s->buffer[pos - 1]))
        return char_color(pos - 1);
    return COL_DEFAULT;
}

static uint8_t word_color(int pos)
{
    static const char* keywords[] = {
        "int", "void", "return", "if", "else", "for", "while",
        "char", "struct", "static", "extern", "typedef", "enum",
        "union", "const", "volatile", "short", "long", "unsigned",
        "signed", "sizeof", "switch", "case", "default", "break",
        "continue", "goto", "do", NULL
    };
    static const char* syscalls[] = {
        "print", "printf", "malloc", "free", "open", "read",
        "write", "close", NULL
    };

    if (pos < 0 || pos >= s->length || !is_alnum(s->buffer[pos]))
        return COL_DEFAULT;

    /* Extract the word */
    char word[32];
    int len = 0;
    int i = pos;
    while (i < s->length && is_alnum(s->buffer[i]) && len < 31) {
        word[len++] = s->buffer[i++];
    }
    word[len] = '\0';

    /* Syscalls */
    for (int k = 0; syscalls[k]; k++) {
        if (strcmp(word, syscalls[k]) == 0)
            return COL_SYSCALL;
    }

    /* Function call heuristic: word followed by '(' */
    int cp = i;
    while (cp < s->length && s->buffer[cp] == ' ')
        cp++;
    if (cp < s->length && s->buffer[cp] == '(')
        return COL_FUNCALL;

    /* Keywords */
    for (int k = 0; keywords[k]; k++) {
        if (strcmp(word, keywords[k]) == 0)
            return COL_KEYWORD;
    }

    return COL_DEFAULT;
}

static uint8_t get_char_color(int pos)
{
    uint8_t c = char_color(pos);
    if (c != COL_DEFAULT)
        return c;

    /* Walk back to start of the current word so keyword/syscall
     * matching works on the full word, not just the remainder. */
    int word_start = pos;
    while (word_start > 0 && is_alnum(s->buffer[word_start - 1]))
        word_start--;

    return word_color(word_start);
}

static void draw_status_bar(void)
{
    /* Clear bar */
    for (int x = 0; x < EDITOR_WIDTH; x++)
        terminal_putentryat(' ', COL_STATUSBG, x, EDITOR_HEIGHT - 1);

    int pos = 0;

    /* Version + filename */
    {
        char ver[] = " Vix 4.0 ";
        for (int i = 0; ver[i]; i++)
            terminal_putentryat(ver[i], COL_STATUSTXT, pos++, EDITOR_HEIGHT - 1);
    }

    /* Filename */
    {
        char sep[] = " ";
        for (int i = 0; sep[i]; i++)
            terminal_putentryat(sep[i], COL_STATUSBG, pos++, EDITOR_HEIGHT - 1);
        for (int i = 0; s->filename[i]; i++)
            terminal_putentryat(s->filename[i], COL_STATUSBG, pos++, EDITOR_HEIGHT - 1);
    }

    /* Dirty marker */
    if (s->dirty) {
        char d[] = " [*]";
        for (int i = 0; d[i]; i++)
            terminal_putentryat(d[i], COL_DIRTY, pos++, EDITOR_HEIGHT - 1);
    }

    /* Line / column */
    {
        char lc[32];
        int lc_len = 0;

        /* Convert line/col numbers manually */
        int line_no = s->cursor_y + 1;
        int col_no  = s->cursor_x + 1;

        char line_str[12], col_str[12];
        int li = 0, ci = 0;

        do { line_str[li++] = '0' + (line_no % 10); line_no /= 10; } while (line_no);
        line_str[li] = '\0';
        /* Reverse */
        for (int a = 0, b = li - 1; a < b; a++, b--) {
            char t = line_str[a]; line_str[a] = line_str[b]; line_str[b] = t;
        }

        do { col_str[ci++] = '0' + (col_no % 10); col_no /= 10; } while (col_no);
        col_str[ci] = '\0';
        for (int a = 0, b = ci - 1; a < b; a++, b--) {
            char t = col_str[a]; col_str[a] = col_str[b]; col_str[b] = t;
        }

        lc[lc_len++] = ' ';
        lc[lc_len++] = 'L';
        lc[lc_len++] = ':';
        for (int i = 0; i < li; i++) lc[lc_len++] = line_str[i];
        lc[lc_len++] = ' ';
        lc[lc_len++] = 'C';
        lc[lc_len++] = ':';
        for (int i = 0; i < ci; i++) lc[lc_len++] = col_str[i];
        lc[lc_len] = '\0';

        for (int i = 0; lc[i]; i++)
            terminal_putentryat(lc[i], COL_STATUSBG, pos++, EDITOR_HEIGHT - 1);
    }

    /* Save status */
    if (s->save_status == 1) {
        char msg[] = " [SAVED]";
        for (int i = 0; msg[i]; i++)
            terminal_putentryat(msg[i], COL_SAVED_OK, pos++, EDITOR_HEIGHT - 1);
    } else if (s->save_status == 2) {
        char msg[] = " [SAVE FAIL]";
        for (int i = 0; msg[i]; i++)
            terminal_putentryat(msg[i], COL_SAVED_FAIL, pos++, EDITOR_HEIGHT - 1);
    }

    /* Transient status message */
    if (s->status_msg[0]) {
        char sep[] = "  ";
        for (int i = 0; sep[i] && pos < EDITOR_WIDTH; i++)
            terminal_putentryat(sep[i], COL_STATUSBG, pos++, EDITOR_HEIGHT - 1);
        for (int i = 0; s->status_msg[i] && pos < EDITOR_WIDTH; i++)
            terminal_putentryat(s->status_msg[i], COL_STATUSBG, pos++, EDITOR_HEIGHT - 1);
    }

    /* Right-aligned help */
    {
        char help[] = " ^S:Save ^B:Build ^Q:Quit ";
        int help_len = 0;
        while (help[help_len]) help_len++;
        int hx = EDITOR_WIDTH - help_len;
        for (int i = 0; help[i]; i++)
            terminal_putentryat(help[i], COL_HELP, hx + i, EDITOR_HEIGHT - 1);
    }
}

static void draw_prompt_line(void)
{
    /* Clear line 0 and use it as prompt area, leaving rest for text */
    for (int x = 0; x < EDITOR_WIDTH; x++)
        terminal_putentryat(' ', COL_PROMPT_BG, x, 0);

    if (s->sub_mode == MODE_SEARCH) {
        char buf[EDITOR_WIDTH];
        int p = 0;

        buf[p++] = '/';
        for (int i = 0; s->search_term[i] && p < EDITOR_WIDTH - 1; i++)
            buf[p++] = s->search_term[i];
        if (p < EDITOR_WIDTH)
            buf[p++] = '_';
        for (; p < EDITOR_WIDTH; p++)
            buf[p] = ' ';

        for (int i = 0; i < EDITOR_WIDTH; i++)
            terminal_putentryat(buf[i], COL_PROMPT_BG, i, 0);

        update_cursor(p < EDITOR_WIDTH ? p - 1 : EDITOR_WIDTH - 1, 0);
    } else if (s->sub_mode == MODE_GOTO) {
        char buf[EDITOR_WIDTH];
        int p = 0;

        buf[p++] = ':';
        for (int i = 0; s->search_term[i] && p < EDITOR_WIDTH - 1; i++)
            buf[p++] = s->search_term[i];
        if (p < EDITOR_WIDTH)
            buf[p++] = '_';
        for (; p < EDITOR_WIDTH; p++)
            buf[p] = ' ';

        for (int i = 0; i < EDITOR_WIDTH; i++)
            terminal_putentryat(buf[i], COL_PROMPT_BG, i, 0);

        update_cursor(p < EDITOR_WIDTH ? p - 1 : EDITOR_WIDTH - 1, 0);
    }
}

static void draw_confirm_dialog(void)
{
    int box_w = 42, box_h = 5;
    int bx = (EDITOR_WIDTH - box_w) / 2;
    int by = (EDITOR_HEIGHT - box_h) / 2;

    for (int y = by; y < by + box_h; y++) {
        for (int x = bx; x < bx + box_w; x++)
            terminal_putentryat(' ', COL_PROMPT_BG, x, y);
    }

    char* msg = "Save modified buffer? (y/n)";
    int msg_len = 0;
    while (msg[msg_len]) msg_len++;
    int mx = bx + (box_w - msg_len) / 2;

    for (int i = 0; msg[i]; i++)
        terminal_putentryat(msg[i], COL_PROMPT_BG, mx + i, by + 2);

    update_cursor(mx + msg_len + 1, by + 2);
}

static void render_text(void)
{
    terminal_initialize();

    if (s->quit_confirm) {
        draw_status_bar();
        draw_confirm_dialog();
        return;
    }

    if (s->sub_mode != MODE_NORMAL) {
        draw_status_bar();
        draw_prompt_line();
        return;
    }

    precompute_syntax();
    draw_status_bar();

    /* Line number width: dynamic (auto-grow for big files) */
    int total_lines = 1;
    for (int i = 0; i < s->length; i++) {
        if (s->buffer[i] == '\n')
            total_lines++;
    }
    int ln_width = LINE_NUM_MIN_WIDTH;
    int t = total_lines;
    while (t >= 1000) { ln_width++; t /= 10; }

    int x_offset = ln_width + 1;    /* +1 for pipe separator */
    int x = x_offset, y = 0;
    int line_num = 1;
    char ln_buf[12];

    for (int i = 0; i < s->length; i++) {
        if (line_num > s->scroll_y && y < VISIBLE_HEIGHT) {
            if (x == x_offset) {
                /* Draw line number in gutter */
                snprintf(ln_buf, sizeof(ln_buf), "%d", line_num);
                int k, lk = 0;
                for (k = 0; k < ln_width; k++)
                    terminal_putentryat(' ', COL_GUTTER, k, y);
                for (k = 0; ln_buf[k]; k++, lk++)
                    terminal_putentryat(ln_buf[k], COL_GUTTER,
                                        ln_width - lk, y);
                terminal_putentryat('|', COL_GUTTER, x_offset - 1, y);
            }

            char c = s->buffer[i];
            if (c != '\n') {
                uint8_t color = get_char_color(i);
                terminal_putentryat(c, color, x, y);
                x++;
                if (x >= EDITOR_WIDTH) {
                    x = x_offset;
                    y++;
                }
            }
        }

        if (s->buffer[i] == '\n') {
            y++;
            x = x_offset;
            line_num++;
        }
        if (y >= VISIBLE_HEIGHT)
            break;
    }

    int cursor_screen_x = s->cursor_x + x_offset;
    int cursor_screen_y = s->cursor_y - s->scroll_y;

    if (cursor_screen_y >= 0 && cursor_screen_y < VISIBLE_HEIGHT)
        update_cursor(cursor_screen_x, cursor_screen_y);
    else
        update_cursor(0, 0);
}

/* Buffer Position (cursor x,y → buffer offset) */

static int get_buffer_pos(int cursor_x, int cursor_y)
{
    int x = 0, y = 0;
    for (int i = 0; i < s->length; i++) {
        if (y == cursor_y && x == cursor_x)
            return i;
        if (s->buffer[i] == '\n') {
            x = 0;
            y++;
        } else {
            x++;
            if (x >= (EDITOR_WIDTH - 4)) {
                x = 0;
                y++;
            }
        }
        if (y > cursor_y)
            return i;
    }
    return s->length;
}

/* Line helpers */

static int line_start(int line)
{
    int l = 0;
    for (int i = 0; i < s->length; i++) {
        if (l == line)
            return i;
        if (s->buffer[i] == '\n')
            l++;
    }
    return s->length;
}

static int line_end(int line)
{
    int l = 0;
    for (int i = 0; i < s->length; i++) {
        if (l == line && s->buffer[i] == '\n')
            return i;
        if (s->buffer[i] == '\n')
            l++;
    }
    return s->length;
}

static int line_length(int line)
{
    return line_end(line) - line_start(line);
}

static int count_lines(void)
{
    int lines = 1;
    for (int i = 0; i < s->length; i++) {
        if (s->buffer[i] == '\n')
            lines++;
    }
    return lines;
}

/* Cursor in bounds */

static void clamp_cursor(void)
{
    int total_lines = count_lines();

    if (s->cursor_y < 0)
        s->cursor_y = 0;
    if (s->cursor_y >= total_lines)
        s->cursor_y = total_lines - 1;
    if (s->cursor_y < 0)
        s->cursor_y = 0;

    int ll = line_length(s->cursor_y);
    if (s->cursor_x > ll)
        s->cursor_x = ll;
    if (s->cursor_x < 0)
        s->cursor_x = 0;
}

/* Undo */

static void save_undo_snapshot(void)
{
    if (!s->undo.has_data && s->length < MAX_FILE_SIZE) {
        s->undo.saved_buffer = (char*)kmalloc(MAX_FILE_SIZE);
        if (!s->undo.saved_buffer)
            return;
        memcpy(s->undo.saved_buffer, s->buffer, s->length);
        s->undo.saved_length = s->length;
        s->undo.saved_cursor_x = s->cursor_x;
        s->undo.saved_cursor_y = s->cursor_y;
        s->undo.saved_scroll_y = s->scroll_y;
        s->undo.has_data = 1;
    }
}

static void perform_undo(void)
{
    if (!s->undo.has_data) {
        set_status_msg("Nothing to undo");
        return;
    }

    /* Swap current and saved buffer */
    char* tmp = s->buffer;
    int tmp_len = s->length;

    s->buffer = s->undo.saved_buffer;
    s->length = s->undo.saved_length;

    s->undo.saved_buffer = tmp;
    s->undo.saved_length = tmp_len;

    s->cursor_x = s->undo.saved_cursor_x;
    s->cursor_y = s->undo.saved_cursor_y;
    s->scroll_y = s->undo.saved_scroll_y;
    s->cursor_x_target = s->cursor_x;

    s->undo.has_data = 0;
    s->dirty = 1;
    clear_status_msg();
    set_status_msg("Undo OK");
}

/* Clipboard */

static void copy_line(void)
{
    int start = line_start(s->cursor_y);
    int end   = line_end(s->cursor_y);

    s->clipboard_len = end - start;
    if (s->clipboard_len > CLIPBOARD_SIZE - 1)
        s->clipboard_len = CLIPBOARD_SIZE - 1;

    memcpy(s->clipboard, s->buffer + start, s->clipboard_len);
    s->clipboard[s->clipboard_len] = '\0';
    set_status_msg("Line copied");
}

static void cut_line(void)
{
    save_undo_snapshot();

    int start = line_start(s->cursor_y);
    int end   = line_end(s->cursor_y);
    int line_len_no_nl = end - start;

    /* Copy to clipboard */
    s->clipboard_len = line_len_no_nl;
    if (s->clipboard_len > CLIPBOARD_SIZE - 1)
        s->clipboard_len = CLIPBOARD_SIZE - 1;
    memcpy(s->clipboard, s->buffer + start, s->clipboard_len);
    s->clipboard[s->clipboard_len] = '\0';

    /* Remove the line + its newline (if any) */
    int remove_end = end;
    if (remove_end < s->length && s->buffer[remove_end] == '\n')
        remove_end++;
    int remove_len = remove_end - start;

    int remaining = s->length - remove_len - start;
    if (remaining > 0)
        memmove_local(s->buffer + start, s->buffer + remove_end, remaining);
    s->length -= remove_len;
    s->buffer[s->length] = '\0';

    if (s->cursor_y >= count_lines())
        s->cursor_y = count_lines() - 1;
    s->cursor_x = 0;
    s->cursor_x_target = 0;
    s->dirty = 1;

    set_status_msg("Line cut");
}

static void paste_clipboard(void)
{
    if (s->clipboard_len <= 0) {
        set_status_msg("Clipboard empty");
        return;
    }

    save_undo_snapshot();

    int pos = get_buffer_pos(s->cursor_x, s->cursor_y);
    int remaining = s->length - pos;

    if (s->length + s->clipboard_len >= MAX_FILE_SIZE) {
        set_status_msg("No room to paste");
        return;
    }

    memmove_local(s->buffer + pos + s->clipboard_len, s->buffer + pos, remaining);
    memcpy(s->buffer + pos, s->clipboard, s->clipboard_len);
    s->length += s->clipboard_len;
    s->buffer[s->length] = '\0';

    /* Advance cursor past pasted text */
    s->cursor_x += s->clipboard_len;
    s->cursor_x_target = s->cursor_x;
    s->dirty = 1;
    set_status_msg("Pasted");
}

/* Search */

static int find_next(const char* needle, int start_pos)
{
    if (!needle || !needle[0])
        return -1;

    int nlen = 0;
    while (needle[nlen]) nlen++;

    for (int i = start_pos; i + nlen <= s->length; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (s->buffer[i + j] != needle[j]) {
                match = 0;
                break;
            }
        }
        if (match)
            return i;
    }
    return -1;
}

static void perform_search(void)
{
    if (!s->search_term[0]) {
        set_status_msg("No search term");
        s->sub_mode = MODE_NORMAL;
        return;
    }

    int start = get_buffer_pos(s->cursor_x, s->cursor_y);
    int found = find_next(s->search_term, start);

    /* Wrap around */
    if (found < 0)
        found = find_next(s->search_term, 0);

    if (found >= 0) {
        /* Navigate cursor to found position */
        int x = 0, y = 0;
        for (int i = 0; i < found; i++) {
            if (s->buffer[i] == '\n') { x = 0; y++; }
            else { x++; }
        }
        s->cursor_x = x;
        s->cursor_y = y;
        s->cursor_x_target = x;

        /* Ensure visible */
        if (s->cursor_y < s->scroll_y)
            s->scroll_y = s->cursor_y;
        if (s->cursor_y >= s->scroll_y + VISIBLE_HEIGHT)
            s->scroll_y = s->cursor_y - VISIBLE_HEIGHT + 1;

        set_status_msg("Found");
    } else {
        set_status_msg("Not found");
    }

    s->sub_mode = MODE_NORMAL;
}

/* Goto Line */

static void perform_goto(void)
{
    int line = 0;
    for (int i = 0; s->search_term[i] >= '0' && s->search_term[i] <= '9'; i++) {
        line = line * 10 + (s->search_term[i] - '0');
    }

    if (line < 1)
        line = 1;

    int total = count_lines();
    if (line > total)
        line = total;

    s->cursor_y = line - 1;
    s->cursor_x = 0;
    s->cursor_x_target = 0;

    /* Scroll to make it visible */
    if (s->cursor_y < s->scroll_y)
        s->scroll_y = s->cursor_y;
    if (s->cursor_y >= s->scroll_y + VISIBLE_HEIGHT)
        s->scroll_y = s->cursor_y - VISIBLE_HEIGHT + 1;

    s->sub_mode = MODE_NORMAL;
}

/* Auto-indent: copy leading whitespace from previous line */

static int get_indent_count(int line)
{
    int start = line_start(line);
    int end   = line_end(line);
    int count = 0;
    for (int i = start; i < end && s->buffer[i] == ' '; i++)
        count++;
    return count;
}

/* Insert character at buffer position */

static void buffer_insert_char(int pos, char c)
{
    if (s->length >= MAX_FILE_SIZE - 1)
        return;

    memmove_local(s->buffer + pos + 1, s->buffer + pos, s->length - pos);
    s->buffer[pos] = c;
    s->length++;
    s->buffer[s->length] = '\0';
}

/* Delete character at buffer position */

static void buffer_delete_at(int pos)
{
    if (pos >= s->length)
        return;
    memmove_local(s->buffer + pos, s->buffer + pos + 1, s->length - pos - 1);
    s->length--;
    s->buffer[s->length] = '\0';
}

/*
 * @brief Fork wrapper for Ctrl+B — compiles and runs the editor buffer via TCC.
 *
 * Called in a forked child task so that if the compiled program exits,
 * only the child dies — not the shell (PID 1).
 */
static void editor_build_wrapper(void* arg)
{
    exec_c_code((const char*)arg, NULL);
}

/* Keyboard Input */

void editor_input(char key)
{
    if (!s || !s->running)
        return;

    /* Sub-mode handling (search / goto) */
    if (s->sub_mode == MODE_SEARCH || s->sub_mode == MODE_GOTO) {
        if (key == 0x1B) {          /* Esc → cancel */
            s->sub_mode = MODE_NORMAL;
            render_text();
            return;
        }
        if (key == '\n') {          /* Enter → execute */
            if (s->sub_mode == MODE_SEARCH)
                perform_search();
            else
                perform_goto();
            render_text();
            return;
        }
        if (key == '\b') {          /* Backspace */
            int len = 0;
            while (s->search_term[len]) len++;
            if (len > 0)
                s->search_term[len - 1] = '\0';
            render_text();
            return;
        }
        /* Regular character → add to prompt */
        if (key >= 32) {
            int len = 0;
            while (s->search_term[len]) len++;
            if (len < SEARCH_TERM_MAX) {
                s->search_term[len] = key;
                s->search_term[len + 1] = '\0';
            }
            render_text();
            return;
        }
        return;
    }

    /* Quit confirmation */
    if (s->quit_confirm) {
        if (key == 'y' || key == 'Y') {
            save_file_internal();
            /* fall through to exit */
        }
        s->quit_confirm = 0;
        if (key == 'y' || key == 'Y' || key == 'n' || key == 'N') {
            editor_running = 0;
            s->running = 0;
            terminal_initialize();
            shell_init();
            return;
        }
        render_text();
        return;
    }

    /* Transient status auto-clear */
    s->save_status = 0;
    s->status_msg[0] = '\0';

    /* Ctrl shortcuts */
    if (key == 0x11) {          /* Ctrl+Q  -  Quit */
        if (s->dirty) {
            s->quit_confirm = 1;
        } else {
            editor_running = 0;
            s->running = 0;
            terminal_initialize();
            shell_init();
            return;
        }
        render_text();
        return;
    }

    if (key == 0x13) {          /* Ctrl+S  -  Save */
        save_file_internal();
        if (s->save_status == 1) {
            /* Clear undo snapshot since file is saved */
            if (s->undo.has_data) {
                kfree(s->undo.saved_buffer);
                s->undo.saved_buffer = NULL;
                s->undo.has_data = 0;
            }
            set_status_msg("Saved");
        }
        render_text();
        return;
    }

    if (key == 0x02) {          /* Ctrl+B  -  Build and Run */
        save_file_internal();
        terminal_initialize();
        terminal_writestring("Compiling ");
        terminal_writestring(s->filename);
        terminal_writestring("\n");

        int pid = kernel_fork(editor_build_wrapper, s->buffer);
        if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        } else {
            terminal_writestring("fork failed\n");
        }

        terminal_writestring("\nPress any key to return to editor...");
        render_text();
        return;
    }

    if (key == 0x1A) {          /* Ctrl+Z  -  Undo */
        perform_undo();
        render_text();
        return;
    }

    if (key == 0x06) {          /* Ctrl+F  -  Search */
        s->sub_mode = MODE_SEARCH;
        s->search_term[0] = '\0';
        render_text();
        return;
    }

    if (key == 0x07) {          /* Ctrl+G  -  Goto Line */
        s->sub_mode = MODE_GOTO;
        s->search_term[0] = '\0';
        render_text();
        return;
    }

    if (key == 0x0E) {          /* Ctrl+N  -  New file */
        s->length = 0;
        s->buffer[0] = '\0';
        s->cursor_x = 0;
        s->cursor_y = 0;
        s->cursor_x_target = 0;
        s->scroll_y = 0;
        s->dirty = 0;
        s->filename[0] = '\0';
        if (s->undo.has_data) {
            kfree(s->undo.saved_buffer);
            s->undo.saved_buffer = NULL;
            s->undo.has_data = 0;
        }
        set_status_msg("New file");
        render_text();
        return;
    }

    if (key == 0x18) {          /* Ctrl+X  -  Cut line */
        cut_line();
        clamp_cursor();
        render_text();
        return;
    }

    if (key == 0x03) {          /* Ctrl+C  -  Copy line */
        copy_line();
        render_text();
        return;
    }

    if (key == 0x16) {          /* Ctrl+V  -  Paste */
        paste_clipboard();
        clamp_cursor();
        render_text();
        return;
    }

    /* Navigation keys */
    if ((unsigned char)key == 0x80) {           /* Up */
        s->cursor_y--;
        if (s->cursor_y < 0) s->cursor_y = 0;
        s->cursor_x = s->cursor_x_target;
        clamp_cursor();
        render_text();
        return;
    }

    if ((unsigned char)key == 0x81) {           /* Down */
        s->cursor_y++;
        int total = count_lines();
        if (s->cursor_y >= total)
            s->cursor_y = total - 1;
        s->cursor_x = s->cursor_x_target;
        clamp_cursor();
        render_text();
        return;
    }

    if ((unsigned char)key == 0x82) {           /* Left */
        if (s->cursor_x > 0) {
            s->cursor_x--;
        } else if (s->cursor_y > 0) {
            s->cursor_y--;
            s->cursor_x = line_length(s->cursor_y);
        }
        s->cursor_x_target = s->cursor_x;
        render_text();
        return;
    }

    if ((unsigned char)key == 0x83) {           /* Right */
        int ll = line_length(s->cursor_y);
        if (s->cursor_x < ll) {
            s->cursor_x++;
        } else if (s->cursor_y + 1 < count_lines()) {
            s->cursor_y++;
            s->cursor_x = 0;
        }
        s->cursor_x_target = s->cursor_x;
        render_text();
        return;
    }

    if ((unsigned char)key == 0x84) {           /* Home */
        s->cursor_x = 0;
        s->cursor_x_target = 0;
        render_text();
        return;
    }

    if ((unsigned char)key == 0x86) {           /* End */
        s->cursor_x = line_length(s->cursor_y);
        s->cursor_x_target = s->cursor_x;
        render_text();
        return;
    }

    if ((unsigned char)key == 0x85) {           /* Page Up */
        s->cursor_y -= VISIBLE_HEIGHT / 2;
        if (s->cursor_y < 0) s->cursor_y = 0;
        s->cursor_x = s->cursor_x_target;
        clamp_cursor();
        s->scroll_y = s->cursor_y;
        render_text();
        return;
    }

    if ((unsigned char)key == 0x87) {           /* Page Down */
        s->cursor_y += VISIBLE_HEIGHT / 2;
        int total = count_lines();
        if (s->cursor_y >= total)
            s->cursor_y = total - 1;
        s->cursor_x = s->cursor_x_target;
        clamp_cursor();
        if (s->cursor_y >= s->scroll_y + VISIBLE_HEIGHT)
            s->scroll_y = s->cursor_y - VISIBLE_HEIGHT + 1;
        render_text();
        return;
    }

    /* Text editing */
    if (key == '\b') {                          /* Backspace */
        save_undo_snapshot();

        int pos = get_buffer_pos(s->cursor_x, s->cursor_y);
        if (pos > 0) {
            if (s->cursor_x > 0) {
                buffer_delete_at(pos - 1);
                s->cursor_x--;
                s->cursor_x_target = s->cursor_x;
            } else if (s->cursor_y > 0) {
                /* Join with previous line */
                int prev_len = line_length(s->cursor_y - 1);
                buffer_delete_at(pos - 1);   /* Delete the '\n' */
                s->cursor_y--;
                s->cursor_x = prev_len;
                s->cursor_x_target = s->cursor_x;
            }
            s->dirty = 1;
        }
        render_text();
        return;
    }

    if (key == '\n') {                          /* Enter */
        save_undo_snapshot();

        int pos = get_buffer_pos(s->cursor_x, s->cursor_y);
        int indent = get_indent_count(s->cursor_y);

        /* Insert newline */
        buffer_insert_char(pos, '\n');
        s->cursor_y++;
        s->cursor_x = 0;

        /* Auto-indent: copy indent from previous line */
        for (int i = 0; i < indent; i++) {
            int new_pos = get_buffer_pos(s->cursor_x, s->cursor_y);
            buffer_insert_char(new_pos, ' ');
            s->cursor_x++;
        }
        s->cursor_x_target = s->cursor_x;
        s->dirty = 1;
        render_text();
        return;
    }

    if (key == '\t') {                          /* Tab → 4 spaces */
        save_undo_snapshot();

        for (int i = 0; i < TAB_SIZE; i++) {
            int pos = get_buffer_pos(s->cursor_x, s->cursor_y);
            buffer_insert_char(pos, ' ');
            s->cursor_x++;
        }
        s->cursor_x_target = s->cursor_x;
        s->dirty = 1;
        render_text();
        return;
    }

    /* Regular printable character */
    if (key >= 32 && key < 127) {
        save_undo_snapshot();

        int pos = get_buffer_pos(s->cursor_x, s->cursor_y);
        buffer_insert_char(pos, key);
        s->cursor_x++;
        if (s->cursor_x >= (EDITOR_WIDTH - 4)) {
            s->cursor_x = 0;
            s->cursor_y++;
        }
        s->cursor_x_target = s->cursor_x;
        s->dirty = 1;
        render_text();
        return;
    }
}

/* Save File */

int save_file_internal(void)
{
    if (!s || !s->filename[0]) {
        s->save_status = 2;
        return -1;
    }

    int fd = fs_open(s->filename, 0);
    if (fd == -1) {
        /* File doesn't exist  -  create it */
        if (fs_create(s->filename) != 0) {
            s->save_status = 2;
            pr_debug("save_file: cannot create %s\n", s->filename);
            return -1;
        }
        fd = fs_open(s->filename, 0);
        if (fd == -1) {
            s->save_status = 2;
            return -1;
        }
    }

    int written = fs_write(fd, s->buffer, s->length);
    fs_close(fd);

    if (written == s->length) {
        s->save_status = 1;
        s->dirty = 0;
        return 0;
    }

    s->save_status = 2;
    return -1;
}

/* Init and Launch */

void start_editor(const char* filename)
{
    /* Allocate editor state */
    if (!s) {
        s = (EditorState*)kmalloc(sizeof(EditorState));
        if (!s) {
            pr_info("start_editor: failed to allocate state\n");
            return;
        }
        memset(s, 0, sizeof(EditorState));
    }

    /* Allocate or reuse buffer */
    if (!s->buffer)
        s->buffer = (char*)kmalloc(MAX_FILE_SIZE);
    if (!s->buffer) {
        pr_info("start_editor: failed to allocate buffer\n");
        return;
    }

    /* Copy filename */
    int fi;
    for (fi = 0; filename[fi] && fi < MAX_FILENAME_LEN; fi++)
        s->filename[fi] = filename[fi];
    s->filename[fi] = '\0';

    /* Load file */
    int fd = fs_open(filename, 0);
    if (fd != -1) {
        s->length = fs_read(fd, s->buffer, MAX_FILE_SIZE - 1);
        fs_close(fd);
    } else {
        s->length = 0;
    }
    s->buffer[s->length] = '\0';

    /* Reset state */
    s->cursor_x = 0;
    s->cursor_y = 0;
    s->cursor_x_target = 0;
    s->scroll_y = 0;
    s->save_status = 0;
    s->quit_confirm = 0;
    s->dirty = 0;
    s->running = 1;
    editor_running = 1;
    s->sub_mode = MODE_NORMAL;
    s->search_term[0] = '\0';
    clear_status_msg();

    /* Reset undo */
    if (s->undo.has_data) {
        kfree(s->undo.saved_buffer);
        s->undo.saved_buffer = NULL;
        s->undo.has_data = 0;
    }

    /* Reset clipboard (but keep content between edits within session) */

    pr_info("Vix 4.0 started: %s (%d bytes)\n", s->filename, s->length);
    render_text();
}
