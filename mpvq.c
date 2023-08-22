#include <err.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <wchar.h>
#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <libgen.h>
#include <pthread.h>
#include <math.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <mpv/client.h>

#define TB_IMPL
#include "termbox2/termbox2.h"

#define FILEEXPLORER_RATIO (2.f/5.f)
#define PLAYLIST_RATIO     (3.f/5.f)
#define MIN_TERMINAL_WIDTH 35
#define MIN_TERMINAL_HEIGHT 15
#define MPVQ_PLIST_HEADER "_MPVQ_PLIST_"

#ifdef __OpenBSD__
#define RAND_FUNCTION arc4random
#else
#define RAND_FUNCTION rand
#endif

static const char *music_file_extensions[] = {
  "mp3", "wav", "ogg", "flac"
}; /* extensions of files recognised as sound files */
static const int n_music_file_extensions = 4;

/*  nw    n    ne
 *   +---------+
 * w |         | e
 *   +---------+
 *  sw    s    es
 *
 * the string is this order: n e s w ne es sw nw */
static const wchar_t *utf8_borderstr  = L"─│─│╮╯╰╭";
static const wchar_t *ascii_borderstr = L"-|-|++++";

typedef enum {
  mode_fileexplorer,
  mode_playlist,
} mode;

typedef enum {
  state_playing,
  state_paused,
  state_nothing_playing
} player_state;

typedef struct {
  int scroll;         /* amount of elements scrolled */
  int cur;            /* element currently pointed at by cursor */
  int n_elems;        /* amount of elements in elems */
  char **elems;       /* elements of the list */
  int x1, y1, x2, y2; /* bounding rect of the list */
} gui_list;

static int fileexplorer_width = -1;
static int playlist_width     = -1;
static int current_playing    = 0; /* "pointer" to currently playing song in */
static char *argv0            = NULL;
static char *cwd              = NULL;
static mpv_handle *ctx        = NULL;
static mode current_mode      = mode_fileexplorer;
static player_state pstate    = state_nothing_playing;
static gui_list playlist;
static gui_list fileexplorer;

#define check_file_error() \
  if (fp == NULL) { \
    char errs[1024]; \
    snprintf(errs, 1024, "file error: %s", strerror(errno)); \
    modal_alert("error", errs); \
    return; \
  } \

#define BASIC_MOVEMENT(T)                                  \
  case L'j':                                               \
    (T).cur += (T).cur + 1 >= (T).n_elems ?  0 : 1; break; \
  case L'k':                                               \
    (T).cur -= (T).cur - 1 < 0 ? 0 : 1; break;             \
  case L'g':                                               \
    (T).cur = 0; break;                                    \
  case L'G':                                               \
    (T).cur = (T).n_elems - 1; break;

#define HANDLE_SCROLL(T)                                        \
  while ((T).y1 + (T).cur - (T).scroll >= (T).y2) (T).scroll++; \
  while ((T).cur < (T).scroll) (T).scroll--;

#define exit_if_term_to_small()                                               \
  if (tb_width() < MIN_TERMINAL_WIDTH || tb_height() < MIN_TERMINAL_HEIGHT) { \
    tb_deinit();                                                              \
    errx(1, "terminal too small");                                            \
  }

#define DEFAULT_MODAL_OPTIONS()                \
  width      = tb_width() / 2;                 \
  height     = (3.f/5.f) * (float)tb_height(); \
  x1         = width / 2;                      \
  x2         = (width / 2) * 3;                \
  y1         = (1.f/5.f) * (float)tb_height(); \
  y2         = y1 * 4;                         \
  max_text_w = width - 2;                      \
  max_text_h = height - 3;                     \
  lines      = ceil(strlen(text) / (float)max_text_w);

#define MODAL_BUFSZ 2048

/* flags */
static int aflag;

static void play_song(char *path) {
  const char *command_load[] = { "loadfile", path, NULL },
             *command_play[] = { "set", "pause", "no", NULL };

  if (path)
    mpv_command(ctx, command_load);
  else
    mpv_command(ctx, command_play);
}

static void pause_song() {
  const char *command[] = { "set", "pause", "yes", NULL };

  mpv_command(ctx, command);
}

static void *event_waiter(void *_) {
  mpv_event *ev;
  (void)_;
  while (1) {
    ev = mpv_wait_event(ctx, 1000);
    switch (ev->event_id) {
      case MPV_EVENT_END_FILE:
        /* this took a while */
        if (((mpv_event_end_file*)ev->data)->reason != MPV_END_FILE_REASON_EOF)
          break;

        if (current_playing + 1 < playlist.n_elems) {
          current_playing++;
          play_song(playlist.elems[current_playing]);
        } else {
          pstate = state_nothing_playing;
          current_playing = 0;
        }
        break;
      default:
        (void)0;
        /* pass */
    }
  }

  return NULL;
}

static pthread_t *init_mpv() {
  int no = 0;
  pthread_t *thr = malloc(sizeof(pthread_t));;

  ctx = mpv_create();
  if (!ctx)
    errx(1, "mpv_create() failed");

  mpv_set_option(ctx, "audio-display", MPV_FORMAT_FLAG, &no);
  mpv_initialize(ctx);

  pthread_create(thr, NULL, event_waiter, NULL);

  return thr;
}

void swap(void **a, void **b) {
  void *tmp = *a;
  *a = *b;
  *b = tmp;
}

/* https://github.com/krzysckh/shuf/blob/master/shuf.c */
void shuf(void **a, int len) {
  int i;

  for (i = len - 1; i > 0; --i)
    swap(&a[(unsigned)RAND_FUNCTION() % len], &a[i]);
}

/* i didn't like how strcoll() sorted my stuff :( */
static int alphabetical(const void *v1, const void *v2) {
  const char *s1 = *(char**)v1,
             *s2 = *(char**)v2;

  if (s1[0] == '.' && s1[1] == '.') return -1;
  if (s2[0] == '.' && s2[1] == '.') return 1;
  if (*s1 && *s2) {
    if (tolower(*s1) > tolower(*s2))
      return 1;
    else if (tolower(*s1) < tolower(*s2))
      return -1;
    else {
      ++s1, ++s2;
      return alphabetical(&s1, &s2);
    }
  } else {
    if (!*s1 && !*s2)
      return 0;
    if (!*s1)
      return -1;
    return 1;
  }
}

static char *getext(char *path) {
  char *p = strrchr(path, '.');
  return p ? ++p : p;
}

static int is_music_ext(char *path) {
  char *ext = getext(path);
  int i;

  if (ext) /* getext returns NULL when file has no extension */
    for (i = 0; i < n_music_file_extensions; ++i)
      if (strcmp(music_file_extensions[i], ext) == 0)
        return 1;
  return 0;
}

static int is_directory(char *path) {
  struct stat s;
  char *realpath = malloc(strlen(cwd) + strlen(path) + 2);
  sprintf(realpath, "%s%s/", cwd, path);

  stat(realpath, &s);
  free(realpath);
  return S_ISDIR(s.st_mode);
}

static void draw_outline(char* title, int x1, int y1, int x2, int y2) {
  const wchar_t *bs = aflag ? ascii_borderstr : utf8_borderstr,
        n = *bs++, e = *bs++, s = *bs++, w = *bs++, ne = *bs++, es = *bs++,
        sw = *bs++, nw = *bs++;
  int i;

  tb_set_cell(x1, y1, nw, TB_WHITE, TB_DEFAULT);
  tb_set_cell(x2, y1, ne, TB_WHITE, TB_DEFAULT);
  tb_set_cell(x2, y2, es, TB_WHITE, TB_DEFAULT);
  tb_set_cell(x1, y2, sw, TB_WHITE, TB_DEFAULT);
  for (i = x1 + 1; i < x2; ++i) {
    tb_set_cell(i, y1, n, TB_WHITE, TB_DEFAULT);
    tb_set_cell(i, y2, s, TB_WHITE, TB_DEFAULT);
  }

  for (i = y1 + 1; i < y2; ++i) {
    tb_set_cell(x1, i, e, TB_WHITE, TB_DEFAULT);
    tb_set_cell(x2, i, w, TB_WHITE, TB_DEFAULT);
  }

  tb_print(x1 + 3, y1, TB_BLUE, TB_DEFAULT, title);
}

static void handle_playpause(int change) {
  if (change) {
    if (pstate == state_playing) {
      pstate = state_paused;
      handle_playpause(0);
    } else if (pstate == state_nothing_playing) {
      pstate = state_playing;
      play_song(playlist.elems[current_playing]);
    } else {
      pstate = state_playing;
      play_song(NULL);
      handle_playpause(0);
    }
  }
}

static void draw_list(gui_list *l, int use_basename, int draw_cursor,
    int draw_playing) {
  int i,
      maxlen = l->x2 - l->x1 - 1,
      maxh = l->y2 - l->y1;
  char *s;
  uintattr_t bg, fg;

  assert(l->x2 > l->x1);
  assert(l->y2 > l->y1);

  for (i = 0; i < (l->n_elems > maxh ? maxh : l->n_elems); ++i) {
    bg = fg = 0;
    s = use_basename ? basename(l->elems[i + l->scroll]) :
      strdup(l->elems[i + l->scroll]);
    if ((int)strlen(s) > maxlen)
      strcpy(s + maxlen - 4, "...");

    if (l->scroll + i == current_playing && draw_playing) {
      if (pstate == state_playing)
        fg = TB_GREEN;
      else
        fg = TB_RED;
    }
    if (draw_cursor && l->scroll + i == l->cur) {
      bg = TB_DEFAULT | TB_REVERSE;
      if (!fg)
        fg = TB_DEFAULT | TB_REVERSE;
    } else {
      bg = TB_DEFAULT;
      if (!fg)
        fg = TB_DEFAULT;
    }

    tb_print(l->x1, l->y1 + i,
        fg, bg, use_basename ? basename(s) : s);
    if (!use_basename)
      free(s);
  }
}

static void playlist_add_song(char *apath) {
  char *path = malloc(strlen(cwd) + strlen(apath) + 1),
       songpath[PATH_MAX];
  int i;
  DIR *dp;
  struct dirent *de;

  sprintf(path, "%s%s", cwd, apath);
  realpath(path, path);

  if (is_directory(apath)) {
    dp = opendir(path);

    if (!dp) {
      tb_deinit();
      err(errno, "cannot opendir(%s)", path);
    }

    while ((de = readdir(dp)) != NULL) {
      if (is_music_ext(de->d_name)) {
        snprintf(songpath, PATH_MAX, "%s/%s", apath, de->d_name);
        playlist_add_song(songpath);
      }
    }

    return;
  }

  /* check if song isn't already in the playlist */
  for (i = 0; i < playlist.n_elems; ++i)
    if (strcmp(path, playlist.elems[i]) == 0)
      return;

  playlist.elems = realloc(playlist.elems,
      sizeof(char*) * (playlist.n_elems + 1));
  playlist.elems[playlist.n_elems] = strdup(path);
  playlist.n_elems++;

  free(path);
}

static void draw_fileexplorer(void) {
  draw_outline("add songs to playlist", 0, 0, fileexplorer_width,
      tb_height() - 1);
  HANDLE_SCROLL(fileexplorer);
  draw_list(&fileexplorer, 0, current_mode == mode_fileexplorer, 0);
}

static void init_fileexplorer() {
  fileexplorer.cur = 0;
  fileexplorer.elems = NULL;
  fileexplorer.n_elems = 0;
  fileexplorer.scroll = 0;
}

static void init_playlist() {
  playlist.cur = 0;
  playlist.elems = NULL;
  playlist.n_elems = 0;
  playlist.scroll = 0;
}

static void modal_alert(char *title, char *text) {
  int width, height, x1, x2, y1, y2, cur_opt = 0, max_text_w, max_text_h, i,
      lines;
  struct tb_event ev;
fully_redraw:
  exit_if_term_to_small();
  DEFAULT_MODAL_OPTIONS();

  assert(lines < max_text_h);

  while (1) {
    tb_clear();
    draw_outline(title, x1, y1, x2, y2);

    /* draw the modal */
    for (i = 0; i < lines; ++i)
      tb_printf(x1 + 1, y1 + 1 + i, TB_YELLOW, TB_DEFAULT, "%.*s", max_text_w,
        text + (i * max_text_w));

    tb_print(x1 + 2, y2 - 2,
        cur_opt == 0 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        cur_opt == 0 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        "[ok]");

    /* wait for input */
    tb_present();
    tb_poll_event(&ev);

    switch (ev.type) {
      case TB_EVENT_RESIZE:
        goto fully_redraw;
        break;
      case TB_EVENT_KEY:
        switch (ev.key) {
          case TB_KEY_ENTER: return;
          default:
            switch (ev.ch) {
              case L'q': return;
            }
            break;
        }
    }
  }
}

/* draw a modal window asking about <text> with title <title> */
static int modal_yn(char *title, char *text) {
  int width, height, x1, x2, y1, y2, cur_opt = 0, max_text_w, max_text_h, i,
      lines;
  struct tb_event ev;
fully_redraw:
  exit_if_term_to_small();
  DEFAULT_MODAL_OPTIONS();

  assert(lines < max_text_h);

  while (1) {
    tb_clear();
    draw_outline(title, x1, y1, x2, y2);

    /* draw the modal */
    for (i = 0; i < lines; ++i)
      tb_printf(x1 + 1, y1 + 1 + i, TB_YELLOW, TB_DEFAULT, "%.*s", max_text_w,
        text + (i * max_text_w));

    tb_print(x1 + 2, y2 - 2,
        cur_opt == 0 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        cur_opt == 0 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        "[no]");

    tb_print(x1 + 8, y2 - 2,
        cur_opt == 1 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        cur_opt == 1 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        "[yes]");

    /* wait for input */
    tb_present();
    tb_poll_event(&ev);

    switch (ev.type) {
      case TB_EVENT_RESIZE:
        goto fully_redraw;
        break;
      case TB_EVENT_KEY:
        switch (ev.key) {
          case TB_KEY_ARROW_LEFT: cur_opt = 0; break;
          case TB_KEY_ARROW_RIGHT: cur_opt = 1; break;
          case TB_KEY_TAB: cur_opt = !cur_opt; break;
          case TB_KEY_ENTER: return cur_opt;
          default:
            switch (ev.ch) {
              case L'y': return 1;
              case L'q': return 0;
              case L'n': return 0;
              case L'h': cur_opt = 0; break;
              case L'l': cur_opt = 1; break;
            }
            break;
        }
    }
  }
}

/* TODO!!: text scrolling so it doesn't overflow */
static char *modal_input(char *title, char *text, char *hint) {
  int width, height, x1, x2, y1, y2, cur_opt = 1, max_text_w, max_text_h, i,
      lines, input_len;
  char *buf = malloc(MODAL_BUFSZ), *buf_start = buf;
  struct tb_event ev;

  memset(buf, 0, MODAL_BUFSZ);
  memcpy(buf, hint, strlen(hint));
  buf += strlen(hint);
fully_redraw:
  exit_if_term_to_small();
  DEFAULT_MODAL_OPTIONS();
  input_len = max_text_w - 15;

  assert(lines < max_text_h);

  while (1) {
    tb_clear();
    draw_outline(title, x1, y1, x2, y2);

    for (i = 0; i < input_len; ++i)
      tb_print(x1 + i + 1, y2 - 2, TB_DEFAULT, TB_DEFAULT, "_");
    tb_print(x1 + 1, y2 - 2, TB_BLUE, TB_DEFAULT, buf_start);

    /* draw the modal */
    for (i = 0; i < lines; ++i)
      tb_printf(x1 + 1, y1 + 1 + i, TB_YELLOW, TB_DEFAULT, "%.*s", max_text_w,
        text + (i * max_text_w));

    tb_print(x2 - 13, y2 - 2,
        cur_opt == 0 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        cur_opt == 0 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        "[cancel]");

    tb_print(x2 - 4, y2 - 2,
        cur_opt == 1 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        cur_opt == 1 ? TB_DEFAULT | TB_REVERSE : TB_DEFAULT,
        "[ok]");

    /* wait for input */
    tb_present();
    tb_poll_event(&ev);

    switch (ev.type) {
      case TB_EVENT_RESIZE:
        goto fully_redraw;
        break;
      case TB_EVENT_KEY:
        switch (ev.key) {
          case TB_KEY_ARROW_LEFT: /* movement in buf */
          case TB_KEY_ARROW_RIGHT: /* movement in buf */ break;
          case TB_KEY_TAB: cur_opt = !cur_opt; break;
          case TB_KEY_ENTER: return cur_opt ? buf_start : NULL;
          case TB_KEY_BACKSPACE:
          case TB_KEY_BACKSPACE2:
            --buf;
            if (buf < buf_start)
              buf = buf_start;
            *buf = 0;
            break;
          default:
            if (ev.ch)
              *buf++ = ev.ch;
            break;
        }
    }
  }
}

static void read_playlist(char *givenpath) {
  char path[PATH_MAX], warnstr[2048], buf[PATH_MAX];
  int i;
  FILE *fp;

  if (givenpath) {
    strncpy(path, givenpath, PATH_MAX);
    goto has_path;
  }

  snprintf(path, PATH_MAX, "%s/%s", realpath(cwd, NULL),
    fileexplorer.elems[fileexplorer.cur]);
  snprintf(warnstr, 2048, "are you sure you want to read %s and overwrite "
      "the current playlist?", path);

  if (!modal_yn("are you sure?", warnstr))
    return;
  if (playlist.n_elems > 0) {
    for (i = 0; i < playlist.n_elems; ++i)
      free(playlist.elems[i]);
    free(playlist.elems);

has_path: /* bit of a hack ig */
    init_playlist();
  }

  fp = fopen(path, "r");
  check_file_error();

  /* header */
  fgets(buf, PATH_MAX, fp);

  if (strncmp(buf, MPVQ_PLIST_HEADER, strlen(MPVQ_PLIST_HEADER)) != 0) {
    modal_alert("error", "this file is not a mpvq playlist");
    return;
  }

  /* size */
  fgets(buf, PATH_MAX, fp);
  playlist.n_elems = atoi(buf);
  playlist.elems = malloc(sizeof(char*) * playlist.n_elems);

  for (i = 0; i < playlist.n_elems; i++) {
    if (feof(fp))
      modal_alert("error", "this playlist file is corrupted");

    fgets(buf, PATH_MAX, fp);
    playlist.elems[i] = strndup(buf, strlen(buf) - 1);
  }

  fclose(fp);
}

static void save_playlist() {
  char *out;
  FILE *fp;
  int i;

  out = modal_input("save playlist to file",
      "enter the desired playlist location:", realpath(cwd, NULL));
  if (out == NULL) return;

  fp = fopen(out, "w");

  check_file_error();

  fprintf(fp, MPVQ_PLIST_HEADER "\n%d\n", playlist.n_elems);
  for (i = 0; i < playlist.n_elems; ++i)
    fprintf(fp, "%s\n", playlist.elems[i]);

  fclose(fp);
}

static void handle_fileexplorer(uint32_t c) {
  char buf[PATH_MAX];
  DIR *dir;
  struct dirent *de;
  int maxl, i;

  if (cwd == NULL) { /* this will run only at start (or when jumped to),
                        when cwd is unset (or if need to change dir) */
    getcwd(buf, PATH_MAX);

    snprintf(buf, PATH_MAX, "%s/", buf);
    cwd = strdup(buf);

    /* FIXME: spaghetti */
change_dir:
    dir = opendir(cwd);
    if (!dir) {
      tb_deinit();
      err(errno, "cannot opendir(%s)", cwd);
    }

    if (fileexplorer.n_elems > 0) {
      for (i = 0; i < fileexplorer.n_elems; ++i)
        free(fileexplorer.elems[i]);
      free(fileexplorer.elems);

      init_fileexplorer();
    }

    /* FIXE: this hurts to look at, fix how it's written or smth idk */
    while ((de = readdir(dir)) != NULL) {
      if (!(de->d_name[0] == '.' && de->d_name[1] != '.')) {
        fileexplorer.elems = realloc(fileexplorer.elems,
          sizeof(char*) * (fileexplorer.n_elems + 1));
        fileexplorer.elems[fileexplorer.n_elems] =
          malloc(strlen(de->d_name) + 2);
        snprintf(fileexplorer.elems[fileexplorer.n_elems],
          strlen(de->d_name) + 2, "%s%s", de->d_name,
          is_directory(de->d_name) ? "/" : "");
        fileexplorer.n_elems++;
      }
    }

    closedir(dir);
    mergesort(fileexplorer.elems, fileexplorer.n_elems, sizeof(char*),
        alphabetical);
  } else { /* normal program loop. interpret commands */
    switch (c) {
      BASIC_MOVEMENT(fileexplorer);
      case L'r':
        read_playlist(NULL);
        break;
      case L'l':
        if (is_directory(fileexplorer.elems[fileexplorer.cur])) {
          maxl = strlen(cwd) + strlen(fileexplorer.elems[fileexplorer.cur]) + 2;
          cwd = realloc(cwd, maxl);
          snprintf(cwd, maxl, "%s%s", cwd,
            fileexplorer.elems[fileexplorer.cur]);
          goto change_dir;
        }
        break;
      case L'a':
        if (is_music_ext(fileexplorer.elems[fileexplorer.cur]) ||
            is_directory(fileexplorer.elems[fileexplorer.cur])) {
          playlist_add_song(fileexplorer.elems[fileexplorer.cur]);
        } else {
          /* TODO: display a popup asking if you're sure */
        }
        break;
    }
  }

  tb_clear();
  draw_fileexplorer();
}

static void draw_playlist(void) {
  draw_outline("playlist", fileexplorer_width + 1, 0,
    fileexplorer_width + playlist_width, tb_height() - 1);
  HANDLE_SCROLL(playlist);
  draw_list(&playlist, 1, current_mode == mode_playlist, 1);
}

static void handle_playlist(uint32_t c) {
  switch (c) {
    BASIC_MOVEMENT(playlist);
    case L'R':
      shuf((void**)playlist.elems, playlist.n_elems);
      break;
    case L'l':
      pstate = state_playing;
      current_playing = playlist.cur;
      play_song(playlist.elems[current_playing]);
      break;

    /* TODO: sorting doesn't work */
    /*case L'r':*/
      /*mergesort(playlist.elems, playlist.n_elems, sizeof(char*), alphabetical);*/
      /*break;*/
    case L'K':
      if (playlist.cur > 0) {
        swap((void**)&playlist.elems[playlist.cur],
             (void**)&playlist.elems[playlist.cur - 1]);
        playlist.cur--;
        current_playing--;
      }
      break;
    case L'J':
      if (playlist.cur + 1 < playlist.n_elems) {
        swap((void**)&playlist.elems[playlist.cur],
             (void**)&playlist.elems[playlist.cur + 1]);
        playlist.cur++;
        current_playing++;
      }
      break;

  }
  draw_playlist();
}


static void ui(void) {
  struct tb_event ev;

  tb_init();
  tb_hide_cursor();

fully_redraw:
  exit_if_term_to_small();
  send_clear(); /* FIXME: this sucks */

  fileexplorer_width = FILEEXPLORER_RATIO * (float)(tb_width() - 1);
  playlist_width = PLAYLIST_RATIO * (float)(tb_width() - 1);

  fileexplorer.x1 = 1;
  fileexplorer.y1 = 1;
  fileexplorer.x2 = fileexplorer_width - 2;
  fileexplorer.y2 = tb_height() - 1;

  playlist.x1 = fileexplorer_width + 2;
  playlist.y1 = 1;
  playlist.x2 = fileexplorer_width + playlist_width - 2;
  playlist.y2 = tb_height() - 1;

  /*draw_fileexplorer();*/
  /*draw_playlist();*/

  while (1) {
    tb_clear();
    handle_fileexplorer(0);
    handle_playlist(0);
    tb_present();

    tb_poll_event(&ev);
    switch (ev.type) {
      case TB_EVENT_RESIZE:
        goto fully_redraw;
        break;
      case TB_EVENT_KEY:
        switch (ev.key) {
          case TB_KEY_CTRL_C:
            goto finish;
            break;
          case TB_KEY_TAB:
            current_mode = current_mode == mode_fileexplorer ? mode_playlist :
              mode_fileexplorer;
            break;
          default: /* it's not a special key, handle it normally */
            switch (ev.ch) {
              case L' ':
                handle_playpause(1);
                if (pstate == state_playing)
                  play_song(NULL);
                else
                  pause_song();
                break;
              case L's':
                save_playlist();
                break;
              case L'q':
                goto finish;
                break;
              case L'n':
                if (current_playing + 1 < playlist.n_elems) {
                  current_playing++;
                  play_song(playlist.elems[current_playing]);
                }
                break;
              case L'N':
                if (current_playing - 1 >= 0) {
                  current_playing--;
                  play_song(playlist.elems[current_playing]);
                }
                break;
              default:
                if (current_mode == mode_fileexplorer)
                  handle_fileexplorer(ev.ch);
                else
                  handle_playlist(ev.ch);
            }
            break;
        }
    }
  }

finish:
  tb_deinit();
}

static void usage() {
  fprintf(stderr, "usage: %s [-a]\n", argv0);
  exit(1);
}

int main(int argc, char *argv[]) {
  int c;
  pthread_t *mpvthr;

  argv0 = *argv;
  while ((c = getopt(argc, argv, "ah")) != -1) {
    switch (c) {
      case 'a':
        aflag = 1;
        break;
      case 'h':
      default:
        usage();
    }
  }

  init_fileexplorer();
  init_playlist();

  if (argv[optind] != NULL)
    read_playlist(argv[optind]);

#if RAND_FUNCTION == rand
  srand(time(0));
#endif

  mpvthr = init_mpv();
  ui();

  pthread_cancel(*mpvthr);
  mpv_terminate_destroy(ctx);
  return 0;
}
