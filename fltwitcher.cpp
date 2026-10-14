// FLTwitcher 0.1 - Cliente de Twitch estilo FreeTube para FLinux-JC /
// FLinux-JC64. C++ + FLTK 1.3. Via anónima (sin cuenta ni claves):
// habla con la misma GraphQL que usa la web de twitch.tv para un
// visitante, con el Client-Id público que la web reparte en su JS
// (verificado en vivo: lista, búsqueda y token salen sin login).
//
//   Directos:    streams(first:20) - los 20 con más espectadores.
//   Canal:       user(login) -> stream (null = no esta en directo;
//                user null = el canal no existe).
//   Sugerencias: searchFor(userQuery) - el buscador de la web; solo
//                valen las candidatas con stream (en vivo). El
//                veredicto lo da user(login) al dar Enter/REPRODUCIR.
//   Categoría:   game(name) -> streams del juego (respaldo cuando
//                el texto no es un canal).
//   Reproduccion: streamPlaybackAccessToken -> master HLS de usher
//                (usher.ttvnw.net/api/channel/hls/{login}.m3u8)
//                -> variante por altura (160/240/360/480/720,
//                defecto 360; exacta, si no la inmediata inferior,
//                si no la más baja) -> ffmpeg demuxa el HLS y
//                entrega MPEG-TS por tubería a mplayer (el mplayer
//                de Tiny Core no abre HLS solo; la receta de
//                FLKicker). Si el master no baja tras los
//                reintentos, va tal cual a ffmpeg («vía master»).
//
// Red reforzada (heredada de FLKicker): timeouts largos (conex.
// 15 s, total 60 s), 2 reintentos con 2 s de pausa en errores
// transitorios, solo IPv4 y NOSIGNAL (obligado con hilos). Los
// errores nombran el host que fallo y el código curl exacto,
// para diagnosticar con una foto.
//
// Color distintivo (peticion de Juanca): lila/morado fluorescente
// de Twitch (familia #A970FF-#BF94FF, nucleo #9146FF), como el
// icono de prismaticos: fila seleccionada, boton REPRODUCIR,
// pulgar activo, boton Favoritos activo, pestana activa de la
// Ayuda y punto «en vivo» de cada fila. Tema oscuro por defecto con cambio a claro; tema,
// calidad y favoritos se guardan en ~/.fltwitcherrc. Sin cuentas
// ni cookies de Twitch en ningun sitio.
//
// Bug del Enter corregido en FLKicker 0.5 y heredado: un
// SearchInput caza FL_KEYDOWN; si se fiara del callback de texto,
// el Enter se lo tragaria FLTK y el usuario se quedaria en el
// filtro local aunque el canal estuviera en vivo.
// Logo: los prismaticos (fltwitcher-48.png del paquete) arriba a
// la izquierda; boton «i» arriba a la derecha abre la «Ayuda de
// FLTwitcher», que se pinta con el tema actual de la app.

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/fl_draw.H>
#include <curl/curl.h>
#include <json-c/json.h>

#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static const char *FLKICK_UA =
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";
// La GraphQL de Twitch con el Client-Id que la web reparte a cada
// visitante en su JS, sin cuenta; usher entrega el master HLS.
static const char *GQL_URL = "https://gql.twitch.tv/gql";
static const char *TW_CID = "kimne78kx3ncx6brgo4mv6wki5h1ko";
static const char *USHER_URL = "https://usher.ttvnw.net/api/channel/hls/";

// ---------------------------------------------------------------- datos
struct Row {
    std::string slug;
    std::string user;
    std::string title;
    long viewers = 0;
    std::string category;
};

// ---------------------------------------------------------------- estado
static bool g_dark = true;        // oscuro por defecto
static int  g_quality = 360;      // calidad por defecto
static const int QUALITIES[] = {160, 240, 360, 480, 720};

// ---- 0.4: favoritos con like (pulgar) y calidad 720p ----
static std::set<std::string> g_likes;  // slugs de canales con like
static bool g_favs_mode = false;       // true: la lista solo muestra likes

static Fl_Window *g_win = nullptr;
static Fl_Input  *g_input = nullptr;
static Fl_Button *g_btn_search = nullptr;
static Fl_Button *g_btn_info = nullptr;
static Fl_Button *g_btn_list = nullptr;
static Fl_Button *g_btn_favs = nullptr;
static Fl_Button *g_btn_theme = nullptr;
static Fl_Button *g_btn_play = nullptr;
static Fl_Button *g_btn_quality = nullptr;
static Fl_Button *g_btn_stop = nullptr;
static Fl_Box    *g_lbl_search = nullptr;
static Fl_Box    *g_lbl_head = nullptr;
static Fl_Box    *g_lbl_player = nullptr;
static Fl_Box    *g_status = nullptr;
static Fl_Box    *g_version = nullptr;

static std::vector<Row> g_rows;   // directos cargados (red, en orden de API)
static std::vector<Row> g_view;   // filas visibles tras el filtro local

// ---- 0.5: sugerencias EN VIVO de la plataforma al teclear ----
static std::vector<Row> g_sugg_rows;    // ultimas sugerencias de la plataforma
static std::string g_sugg_for;          // texto (minusculas) al que responden
static std::string g_sugg_inflight;     // texto con un job ya volando
static std::string g_sugg_queued;       // texto esperando al debounce
static double g_last_key_t = 0;         // instante del ultimo tecleo

static pid_t  g_player_pid = -1;
static double g_player_t0 = 0;
static bool   g_user_stopped = false;
static std::string g_playing_label;

// ---------------------------------------------------------------- config
static std::string rc_path() {
    const char *h = getenv("HOME");
    return std::string(h ? h : "/tmp") + "/.fltwitcherrc";
}

static std::string trim_line(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    std::string out = s ? s : "";
    while (!out.empty()) {
        char c = out[out.size() - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            out.erase(out.size() - 1);
        else break;
    }
    return out;
}

static void cfg_load() {
    FILE *f = fopen(rc_path().c_str(), "r");
    if (!f) return;
    char ln[128];
    while (fgets(ln, sizeof(ln), f)) {
        if (strncmp(ln, "tema=claro", 10) == 0) g_dark = false;
        else if (strncmp(ln, "tema=oscuro", 11) == 0) g_dark = true;
        else if (strncmp(ln, "calidad=", 8) == 0) {
            int q = atoi(ln + 8);
            if (q == 160 || q == 240 || q == 360 || q == 480 || q == 720)
                g_quality = q;
        }
        else if (strncmp(ln, "like=", 5) == 0) {
            std::string s = trim_line(ln + 5);
            if (!s.empty()) g_likes.insert(s);
        }
    }
    fclose(f);
}

static void cfg_save() {
    FILE *f = fopen(rc_path().c_str(), "w");
    if (!f) return;
    fprintf(f, "tema=%s\ncalidad=%d\n", g_dark ? "oscuro" : "claro", g_quality);
    for (std::set<std::string>::const_iterator it = g_likes.begin();
         it != g_likes.end(); ++it)
        fprintf(f, "like=%s\n", it->c_str());
    fclose(f);
}

// ---------------------------------------------------------------- tema
struct Palette {
    Fl_Color win, input_bg, txt, btn, header, row, row_alt, sel, sel_txt;
};
static const Palette PAL_DARK = {
    fl_rgb_color(20, 22, 29),  fl_rgb_color(29, 32, 39),
    fl_rgb_color(230, 230, 230), fl_rgb_color(35, 37, 45),
    fl_rgb_color(38, 40, 47),
    fl_rgb_color(24, 26, 33),  fl_rgb_color(28, 30, 38),
    fl_rgb_color(88, 50, 150),  fl_rgb_color(255, 255, 255)};
static const Palette PAL_LIGHT = {
    fl_rgb_color(240, 240, 240), fl_rgb_color(255, 255, 255),
    fl_rgb_color(25, 25, 25),  fl_rgb_color(224, 224, 224),
    fl_rgb_color(232, 232, 232),
    fl_rgb_color(255, 255, 255), fl_rgb_color(245, 245, 245),
    fl_rgb_color(145, 70, 255), fl_rgb_color(255, 255, 255)};
static const Palette &pal() { return g_dark ? PAL_DARK : PAL_LIGHT; }
static Fl_Color tw_purple() { return fl_rgb_color(145, 70, 255); }
// nucleo Twitch
static Fl_Color tw_lilac() { return fl_rgb_color(169, 112, 255); }
// lila neon del icono (acento)
// Punto «en vivo» de cada fila: el mismo lila neon distintivo.
// En oscuro brilla el fluorescente; en claro manda el nucleo, más
// rico sobre blanco; sobre la fila seleccionada (fondo morado en
// ambos temas) va el más claro de la familia para que se lea.
static Fl_Color live_dot_color(bool sel) {
    if (sel) return fl_rgb_color(191, 148, 255);  // #BF94FF
    return g_dark ? tw_lilac() : tw_purple();
}

// ---------------------------------------------------------------- http
struct HttpRes {
    long code = 0;
    int curl_err = 0;
    int attempts = 1;      // intentos reales que costo la peticion
    std::string url;       // URL pedida (diagnostico en la barra)
    std::string body;
};

static size_t wr_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    ((std::string *)ud)->append((const char *)ptr, sz * nm);
    return sz * nm;
}

static std::string host_of(const std::string &url) {
    size_t p = url.find("://");
    if (p == std::string::npos) return url;
    size_t h = url.find('/', p + 3);
    size_t n = (h == std::string::npos ? url.size() : h) - (p + 3);
    return url.substr(p + 3, n);
}

// Errores de transporte que vale la pena reintentar (DNS que parpadea,
// conexión rechazada, tiempo agotado, corte a mitad): los que mataban
// el REPRODUCIR en la Dell con un curl 28 solitario.
static bool curl_transitorio(CURLcode cc) {
    return cc == CURLE_COULDNT_RESOLVE_HOST || cc == CURLE_COULDNT_CONNECT ||
           cc == CURLE_OPERATION_TIMEDOUT  || cc == CURLE_GOT_NOTHING ||
           cc == CURLE_RECV_ERROR;
}

// GET con red reforzada: timeouts largos (conex. 15 s, total 60 s),
// 2 reintentos con 2 s de pausa, solo IPv4 y NOSIGNAL (imprescindible
// para timeouts con DNS dentro de un hilo). xh_name/xh_val anaden una
// cabecera extra (la clave publica de Typesense en las sugerencias).
static HttpRes http_get(const std::string &url, const char *xh_name = nullptr,
                        const char *xh_val = nullptr) {
    HttpRes r;
    r.url = url;
    for (int intento = 1; intento <= 3; intento++) {
        r.attempts = intento;
        r.code = 0;
        r.curl_err = 0;
        r.body.clear();
        CURL *c = curl_easy_init();
        if (!c) { r.curl_err = -1; return r; }
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(c, CURLOPT_USERAGENT, FLKICK_UA);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        struct curl_slist *hdr = nullptr;
        hdr = curl_slist_append(hdr, "Accept: application/json");
        hdr = curl_slist_append(hdr, "Accept-Language: es-ES,es;q=0.9");
        if (xh_name && xh_val) {
            std::string xh = std::string(xh_name) + ": " + xh_val;
            hdr = curl_slist_append(hdr, xh.c_str());
        }
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
        CURLcode cc = curl_easy_perform(c);
        if (cc == CURLE_OK)
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.code);
        else
            r.curl_err = (int)cc;
        curl_slist_free_all(hdr);
        curl_easy_cleanup(c);
        if (cc == CURLE_OK || !curl_transitorio(cc) || intento == 3)
            return r;
        sleep(2);  // pausa entre reintentos (corre en el hilo del job)
    }
    return r;
}

// POST JSON con la misma red reforzada (la GraphQL de Twitch pide
// POST; xh_name/xh_val anaden una cabecera, p. ej. el Client-Id).
static HttpRes http_post(const std::string &url, const char *body_post,
                         const char *xh_name = nullptr,
                         const char *xh_val = nullptr) {
    HttpRes r;
    r.url = url;
    for (int intento = 1; intento <= 3; intento++) {
        r.attempts = intento;
        r.code = 0;
        r.curl_err = 0;
        r.body.clear();
        CURL *c = curl_easy_init();
        if (!c) { r.curl_err = -1; return r; }
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(c, CURLOPT_USERAGENT, FLKICK_UA);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_post);
        struct curl_slist *hdr = nullptr;
        hdr = curl_slist_append(hdr, "Content-Type: application/json");
        hdr = curl_slist_append(hdr, "Accept: application/json");
        if (xh_name && xh_val) {
            std::string xh = std::string(xh_name) + ": " + xh_val;
            hdr = curl_slist_append(hdr, xh.c_str());
        }
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
        CURLcode cc = curl_easy_perform(c);
        if (cc == CURLE_OK)
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.code);
        else
            r.curl_err = (int)cc;
        curl_slist_free_all(hdr);
        curl_easy_cleanup(c);
        if (cc == CURLE_OK || !curl_transitorio(cc) || intento == 3)
            return r;
        sleep(2);  // pausa entre reintentos (corre en el hilo del job)
    }
    return r;
}

static std::string lower_of(std::string s) {
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32;
    return s;
}

static bool is_cloudflare(const HttpRes &r) {
    if (r.code != 403 && r.code != 503 && r.code != 429) return false;
    std::string b = lower_of(r.body.substr(0, 8192));
    return b.find("cloudflare") != std::string::npos ||
           b.find("challenge") != std::string::npos ||
           b.find("just a moment") != std::string::npos ||
           b.find("cf-ray") != std::string::npos;
}

static std::string net_error(const HttpRes &r, const char *que) {
    char buf[384];
    std::string host = host_of(r.url);
    if (r.curl_err)
        // Diagnostico con una foto: host que fallo y código curl exacto.
        snprintf(buf, sizeof(buf),
                 "Sin conexion con Twitch (%s): fallo %s "
                 "(curl %d, %d intentos).",
                 que, host.c_str(), r.curl_err, r.attempts);
    else if (is_cloudflare(r))
        snprintf(buf, sizeof(buf),
                 "La red rechazo la peticion desde esta IP (HTTP %ld). "
                 "Intenta de nuevo en un rato.", r.code);
    else
        snprintf(buf, sizeof(buf), "Twitch respondio HTTP %ld en %s (%s).",
                 r.code, que, host.c_str());
    return buf;
}

// ---------------------------------------------------------------- json
static const char *js_str(json_object *o, const char *k) {
    json_object *v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v) {
        const char *s = json_object_get_string(v);
        return s ? s : "";
    }
    return "";
}
static long js_int(json_object *o, const char *k) {
    json_object *v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v)
        return json_object_get_int64(v);
    return 0;
}
static json_object *js_obj(json_object *o, const char *k) {
    json_object *v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v)) return v;
    return nullptr;
}

// ---------------------------------------------------------------- trabajos
enum JobKind { JOB_LIST, JOB_CHANNEL, JOB_CATEGORY, JOB_PLAY, JOB_SUGGEST };

struct JobResult {
    JobKind kind;
    bool ok = false;
    std::string query;   // texto que origino el job (sugerencias)
    bool notfound = false;
    std::string err;
    std::string note;
    std::vector<Row> rows;
    std::string play_url;
    int play_height = 0;
    std::string play_label;
    bool via_master = false;  // la URL es el master: ffmpeg lo resuelve
};

struct JobArg {
    JobKind kind;
    std::string query;
};

static void awake_cb(void *p);
static void run_job(JobKind kind, const std::string &q, const char *msg,
                    bool silent = false);

// ---------------------------------------------------------- backend
// Todo pasa por la GraphQL de Twitch con el Client-Id de la web
// (sin cuenta). Las consultas usan variables: los logins de Twitch
// solo admiten [a-z0-9_] y los textos se saneam antes del JSON.
static HttpRes tw_gql(const std::string &body) {
    return http_post(GQL_URL, body.c_str(), "Client-Id", TW_CID);
}

static std::string qvar(const std::string &v) {  // valor JSON seguro
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        char c = v[i];
        if (c != '"' && c != '\\' && c >= 32) o += c;
    }
    return o;
}

static bool js_live(json_object *st) {  // stream presente y no null
    return st && json_object_get_type(st) != json_type_null;
}

static const char *gql_err(json_object *root) {
    if (!root) return "";
    json_object *errs = js_obj(root, "errors");
    if (!errs || json_object_get_type(errs) != json_type_array ||
        !json_object_array_length(errs))
        return "";
    return js_str(json_object_array_get_idx(errs, 0), "message");
}

static void row_de_node(json_object *node, Row &r) {
    json_object *b = js_obj(node, "broadcaster");
    json_object *g = js_obj(node, "game");
    r.slug = js_str(b, "login");
    r.user = js_str(b, "displayName");
    if (r.user.empty()) r.user = r.slug;
    r.title = js_str(node, "title");
    r.viewers = js_int(node, "viewersCount");
    r.category = js_str(g, "displayName");
}

static void rows_de_edges(json_object *edges, const char *campo,
                          std::vector<Row> &out) {
    if (!edges || json_object_get_type(edges) != json_type_array) return;
    size_t n = json_object_array_length(edges);
    for (size_t i = 0; i < n; i++) {
        json_object *nd =
            js_obj(json_object_array_get_idx(edges, i), campo);
        if (!nd) continue;
        Row r;
        row_de_node(nd, r);
        if (!r.slug.empty()) out.push_back(r);
    }
}

// ---------------------------------------------------------------- HLS
struct Variant { int height; std::string url; };

static std::string resolve_url(const std::string &base, const std::string &rel) {
    if (rel.find("://") != std::string::npos) return rel;
    std::string b = base.substr(0, base.find('?'));
    if (!rel.empty() && rel[0] == '/') {
        size_t p = b.find("://");
        size_t h = (p == std::string::npos) ? 0 : b.find('/', p + 3);
        return (h == std::string::npos ? b : b.substr(0, h)) + rel;
    }
    size_t s = b.rfind('/');
    return (s == std::string::npos ? b : b.substr(0, s + 1)) + rel;
}

static void parse_variants(const std::string &master, const std::string &base,
                           std::vector<Variant> &out) {
    std::string pending;
    int pending_h = 0;
    size_t pos = 0;
    while (pos <= master.size()) {
        size_t nl = master.find('\n', pos);
        std::string ln = master.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!ln.empty() && ln[ln.size() - 1] == '\r') ln.erase(ln.size() - 1);
        pos = (nl == std::string::npos) ? master.size() + 1 : nl + 1;
        if (ln.find("#EXT-X-STREAM-INF:") == 0) {
            pending = ln;
            pending_h = 0;
            size_t rp = ln.find("RESOLUTION=");
            if (rp != std::string::npos) {
                int w = 0, h = 0;
                if (sscanf(ln.c_str() + rp + 11, "%dx%d", &w, &h) == 2)
                    pending_h = h;
            }
            if (!pending_h) {  // respaldo: NAME="480p"
                size_t np = ln.find("NAME=\"");
                if (np != std::string::npos) pending_h = atoi(ln.c_str() + np + 6);
            }
        } else if (!ln.empty() && ln[0] != '#' && !pending.empty()) {
            if (pending_h > 0) {
                Variant v;
                v.height = pending_h;
                v.url = resolve_url(base, ln);
                out.push_back(v);
            }
            pending.clear();
        }
    }
}

// exacta; si no, inmediata inferior; si tampoco, la más baja
static const Variant *choose_variant(const std::vector<Variant> &v, int want) {
    const Variant *exact = nullptr, *lower = nullptr, *lowest = nullptr;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].height == want) exact = &v[i];
        if (v[i].height < want && (!lower || v[i].height > lower->height))
            lower = &v[i];
        if (!lowest || v[i].height < lowest->height) lowest = &v[i];
    }
    return exact ? exact : (lower ? lower : lowest);
}

static void *job_thread(void *vp) {
    JobArg *arg = (JobArg *)vp;
    JobResult *res = new JobResult();
    res->kind = arg->kind;

    if (arg->kind == JOB_LIST) {
        HttpRes r = tw_gql(
            "{\"query\":\"query{streams(first:20){edges{node{title "
            "viewersCount broadcaster{login displayName} "
            "game{displayName}}}}}\"}");
        if (r.curl_err || r.code != 200) {
            res->err = net_error(r, "el listado de directos");
        } else {
            json_object *root = json_tokener_parse(r.body.c_str());
            const char *ge = gql_err(root);
            if (!root)
                res->err = "Respuesta de Twitch no valida (JSON).";
            else if (*ge)
                res->err = std::string("Twitch: ") + ge;
            else {
                json_object *st =
                    js_obj(js_obj(root, "data"), "streams");
                rows_de_edges(js_obj(st, "edges"), "node", res->rows);
                res->ok = true;
                char b[96];
                snprintf(b, sizeof(b),
                         "%zu directos en vivo - via anonima, sin cuenta",
                         res->rows.size());
                res->note = b;
            }
            if (root) json_object_put(root);
        }
    } else if (arg->kind == JOB_CHANNEL || arg->kind == JOB_PLAY) {
        // Comprobacion exacta: user(login) trae el directo o null.
        std::string login = arg->query;
        std::string body =
            "{\"query\":\"query($l:String!){user(login:$l){login "
            "displayName stream{title viewersCount "
            "game{displayName}}}}\",\"variables\":{\"l\":\"" +
            qvar(login) + "\"}}";
        HttpRes r = tw_gql(body);
        if (r.curl_err || r.code != 200) {
            res->err = net_error(r, "el canal");
        } else {
            json_object *root = json_tokener_parse(r.body.c_str());
            const char *ge = gql_err(root);
            if (!root) {
                res->err = "Respuesta de Twitch no valida (JSON).";
            } else if (*ge) {
                res->err = std::string("Twitch: ") + ge;
            } else {
                json_object *u = js_obj(js_obj(root, "data"), "user");
                if (!u || json_object_get_type(u) == json_type_null) {
                    res->notfound = true;
                    res->err = "El canal buscado no existe en Twitch.";
                } else {
                    std::string user = js_str(u, "displayName");
                    if (user.empty()) user = js_str(u, "login");
                    if (user.empty()) user = login;
                    json_object *st = js_obj(u, "stream");
                    if (!js_live(st)) {
                        res->err =
                            "Ese canal no esta en directo ahora.";
                        res->note = user;
                    } else if (arg->kind == JOB_CHANNEL) {
                        Row row;
                        row.slug = login;
                        row.user = user;
                        row.title = js_str(st, "title");
                        row.viewers = js_int(st, "viewersCount");
                        row.category =
                            js_str(js_obj(st, "game"), "displayName");
                        res->rows.push_back(row);
                        res->ok = true;
                        res->note = "\xc2«" + user +
                                    "\xc2» esta en directo ahora.";
                    } else {  // JOB_PLAY: token + master de usher
                        std::string tb =
                            "{\"query\":\"query($c:String!){"
                            "streamPlaybackAccessToken(channelName:$c,"
                            "params:{platform:\\\"web\\\","
                            "playerBackend:\\\"mediaplayer\\\","
                            "playerType:\\\"site\\\"}){value "
                            "signature}}\",\"variables\":{\"c\":\"" +
                            qvar(login) + "\"}}";
                        HttpRes t = tw_gql(tb);
                        std::string tval, tsig;
                        json_object *troot = nullptr;
                        if (!t.curl_err && t.code == 200)
                            troot = json_tokener_parse(t.body.c_str());
                        json_object *tok =
                            troot ? js_obj(js_obj(troot, "data"),
                                           "streamPlaybackAccessToken")
                                  : nullptr;
                        if (tok) {
                            tval = js_str(tok, "value");
                            tsig = js_str(tok, "signature");
                        }
                        if (tval.empty() || tsig.empty()) {
                            res->err = "Twitch no entrego el token de "
                                       "reproduccion del canal.";
                        } else {
                            CURL *ce = curl_easy_init();
                            char *et = ce ? curl_easy_escape(
                                                ce, tval.c_str(), 0)
                                          : nullptr;
                            char *es = ce ? curl_easy_escape(
                                                ce, tsig.c_str(), 0)
                                          : nullptr;
                            std::string murl =
                                std::string(USHER_URL) + login +
                                ".m3u8?token=" + (et ? et : "") +
                                "&sig=" + (es ? es : "") +
                                "&allow_source=true";
                            if (et) curl_free(et);
                            if (es) curl_free(es);
                            if (ce) curl_easy_cleanup(ce);
                            HttpRes m = http_get(murl);
                            bool have_variant = false;
                            if (!m.curl_err && m.code == 200) {
                                std::vector<Variant> vars;
                                parse_variants(m.body, murl, vars);
                                if (!vars.empty()) {
                                    const Variant *v = choose_variant(
                                        vars, g_quality);
                                    res->ok = true;
                                    res->play_url = v->url;
                                    res->play_height = v->height;
                                    char lb[160];
                                    snprintf(lb, sizeof(lb),
                                             "%s - %dp", user.c_str(),
                                             v->height);
                                    res->play_label = lb;
                                    have_variant = true;
                                }
                            }
                            if (!have_variant) {
                                // Fallback: el master va tal cual a
                                // ffmpeg y el resuelve el HLS.
                                res->ok = true;
                                res->via_master = true;
                                res->play_url = murl;
                                res->play_height = 0;
                                res->play_label = user;
                            }
                        }
                        if (troot) json_object_put(troot);
                    }
                }
            }
            if (root) json_object_put(root);
        }
    } else if (arg->kind == JOB_CATEGORY) {
        // Categoría = juego de Twitch: game(name) trae sus directos.
        std::string body =
            "{\"query\":\"query($n:String!){game(name:$n){"
            "displayName streams(first:20){edges{node{title "
            "viewersCount broadcaster{login "
            "displayName}}}}}}\",\"variables\":{\"n\":\"" +
            qvar(arg->query) + "\"}}";
        HttpRes r = tw_gql(body);
        if (r.curl_err || r.code != 200) {
            res->err = net_error(r, "la categoria");
        } else {
            json_object *root = json_tokener_parse(r.body.c_str());
            const char *ge = gql_err(root);
            if (!root)
                res->err = "Respuesta de Twitch no valida (JSON).";
            else if (*ge)
                res->err = std::string("Twitch: ") + ge;
            else {
                json_object *g = js_obj(js_obj(root, "data"), "game");
                if (g && json_object_get_type(g) != json_type_null) {
                    rows_de_edges(
                        js_obj(js_obj(g, "streams"), "edges"), "node",
                        res->rows);
                    // El nodo no trae game: la categoría es el
                    // juego buscado (la columna no queda vacia).
                    std::string gn = js_str(g, "displayName");
                    if (gn.empty()) gn = arg->query;
                    for (size_t i = 0; i < res->rows.size(); i++)
                        if (res->rows[i].category.empty())
                            res->rows[i].category = gn;
                }
                res->ok = true;
                char b[192];
                snprintf(b, sizeof(b),
                         "%zu directos para \xc2«%s\xc2» "
                         "(categoria de Twitch).",
                         res->rows.size(), arg->query.c_str());
                res->note = b;
            }
            if (root) json_object_put(root);
        }
    } else if (arg->kind == JOB_SUGGEST) {
        // Sugerencias EN VIVO de toda la plataforma: searchFor, el
        // buscador de la web. Solo candidatas con stream (en vivo);
        // el veredicto lo da user(login) al dar Enter/REPRODUCIR.
        res->query = arg->query;
        std::string body =
            "{\"query\":\"query($q:String!){searchFor(userQuery:$q,"
            "platform:\\\"web\\\"){channels{edges{item{... on "
            "User{login displayName stream{title viewersCount "
            "game{displayName}}}}}}}}\",\"variables\":{\"q\":\"" +
            qvar(arg->query) + "\"}}";
        HttpRes r = tw_gql(body);
        if (!r.curl_err && r.code == 200) {
            json_object *root = json_tokener_parse(r.body.c_str());
            if (root && !*gql_err(root)) {
                json_object *ch =
                    js_obj(js_obj(js_obj(root, "data"), "searchFor"),
                           "channels");
                json_object *edges = js_obj(ch, "edges");
                std::set<std::string> seen;
                if (edges &&
                    json_object_get_type(edges) == json_type_array) {
                    size_t n = json_object_array_length(edges);
                    for (size_t i = 0; i < n; i++) {
                        json_object *it = js_obj(
                            json_object_array_get_idx(edges, i),
                            "item");
                        if (!it) continue;
                        json_object *st = js_obj(it, "stream");
                        if (!js_live(st)) continue;  // apagado fuera
                        Row row;
                        row.slug = js_str(it, "login");
                        row.user = js_str(it, "displayName");
                        if (row.user.empty()) row.user = row.slug;
                        row.title = js_str(st, "title");
                        row.viewers = js_int(st, "viewersCount");
                        row.category =
                            js_str(js_obj(st, "game"), "displayName");
                        if (row.slug.empty() || seen.count(row.slug))
                            continue;
                        seen.insert(row.slug);
                        res->rows.push_back(row);
                    }
                }
                res->ok = true;  // tambien si salen 0: no es un error
            }
            if (root) json_object_put(root);
        }
    }

    Fl::awake(awake_cb, res);
    delete arg;
    return nullptr;
}

// ---------------------------------------------------------------- player
static void set_status(const std::string &s) {
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s", s.c_str());
    g_status->label(buf);
    g_status->redraw();
}

static double now_s() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void player_launch(const std::string &url, const std::string &label,
                          bool via_master = false) {
    if (g_player_pid > 0) {
        kill(-g_player_pid, SIGTERM);
        waitpid(g_player_pid, nullptr, 0);
        g_player_pid = -1;
    }
    // Con el fallback el master va pelado a ffmpeg y la calidad ya no
    // esta forzada: la etiqueta lo deja claro en todo el estado.
    g_playing_label = via_master ? label + " (via master)" : label;
    // Tuberia ffmpeg -> mplayer (ver el comentario de cabecera): ffmpeg
    // demuxa el HLS por HTTPS con reconexiones y entrega MPEG-TS limpio;
    // mplayer solo decodifica y muestra. Ambos procesos quedan en el
    // mismo grupo (setsid) para que Detener mate los dos a la vez.
    std::string uq = url;
    for (size_t i = 0; (i = uq.find('\'', i)) != std::string::npos; i += 4)
        uq.replace(i, 1, "'\\''");
    char cmdb[4096];
    snprintf(cmdb, sizeof(cmdb),
             "ffmpeg -hide_banner -loglevel error -user_agent '%s' "
             "-reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 "
             "-i '%s' -c copy -f mpegts pipe:1 | "
             "exec mplayer -demuxer lavf "
             "-lavfdopts format=mpegts:probesize=200000 "
             "-vo xv,x11 -ao alsa,pulse,null -cache 8192 "
             "-cache-min 20 -framedrop -",
             FLKICK_UA, uq.c_str());
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        FILE *dn = fopen("/dev/null", "w");
        if (dn) { dup2(fileno(dn), 1); dup2(fileno(dn), 2); }
        execlp("/bin/sh", "sh", "-c", cmdb, (char *)nullptr);
        _exit(127);
    }
    if (pid > 0) {
        g_player_pid = pid;
        g_player_t0 = now_s();
        g_user_stopped = false;
        set_status(via_master
                       ? std::string("Reproduciendo via master: ") + label
                       : std::string("Reproduciendo: ") + label);
    } else {
        set_status("No se pudo lanzar mplayer.");
    }
}

static void player_stop(bool silent) {
    if (g_player_pid > 0) {
        g_user_stopped = true;
        kill(-g_player_pid, SIGTERM);
        waitpid(g_player_pid, nullptr, 0);
        g_player_pid = -1;
        if (!silent) set_status("Detenido.");
    }
}

static void watch_cb(void *) {
    // Debounce de sugerencias: ~0,5 s despues del último tecleo (este
    // watch pasa cada 0,5 s) y sin solapar dos JOB_SUGGEST.
    if (!g_sugg_queued.empty() && g_sugg_inflight.empty() &&
        g_sugg_queued != g_sugg_for && now_s() - g_last_key_t >= 0.5) {
        g_sugg_inflight = g_sugg_queued;
        run_job(JOB_SUGGEST, g_sugg_queued, "", true);
    }
    if (g_player_pid > 0) {
        int st = 0;
        pid_t r = waitpid(g_player_pid, &st, WNOHANG);
        if (r == g_player_pid) {
            double up = now_s() - g_player_t0;
            std::string lbl = g_playing_label;
            g_player_pid = -1;
            if (g_user_stopped) {
                // ya informado
            } else if (up < 6.0) {
                set_status(std::string("No se pudo reproducir ") + lbl +
                           ". Revisa la conexion.");
            } else {
                set_status(std::string("Fin de la reproduccion: ") + lbl);
            }
        }
    }
    Fl::repeat_timeout(0.5, watch_cb);
}

// ---------------------------------------------------------------- tabla
static std::string fmt_viewers(long n) {
    char raw[32];
    snprintf(raw, sizeof(raw), "%ld", n);
    std::string s = raw, out;
    int cnt = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) {
        out.insert(out.begin(), s[i]);
        if (++cnt % 3 == 0 && i > 0) out.insert(out.begin(), ' ');
    }
    return out;
}

// Pulgar arriba dibujado (la fuente del banco no garantiza el emoji):
// inactivo = silueta clara con contorno negro grueso, como el PNG;
// activo = relleno lila neon, el acento de FLTwitcher.
static void draw_thumb(int X, int Y, int W, int H, bool active) {
    static const double pts[][2] = {
        {5, 21}, {5, 12}, {8, 12}, {10, 4}, {13, 4}, {15, 7},
        {14, 11}, {20, 11}, {21, 13}, {20.5, 16}, {21, 18}, {20, 21},
    };
    const int cx = X + (W - 24) / 2, cy = Y + (H - 24) / 2;
    fl_color(active ? tw_lilac() : fl_rgb_color(238, 238, 238));
    fl_begin_polygon();
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++)
        fl_vertex(cx + pts[i][0], cy + pts[i][1]);
    fl_end_polygon();
    fl_color(fl_rgb_color(10, 10, 10));
    fl_line_style(FL_SOLID, 2);
    fl_begin_loop();
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++)
        fl_vertex(cx + pts[i][0], cy + pts[i][1]);
    fl_end_loop();
    fl_line_style(FL_SOLID, 1);
    fl_begin_line();
    fl_vertex(cx + 15.5, cy + 14.5);
    fl_vertex(cx + 20, cy + 14.5);
    fl_end_line();
    fl_begin_line();
    fl_vertex(cx + 15.5, cy + 17.5);
    fl_vertex(cx + 20, cy + 17.5);
    fl_end_line();
}

static void toggle_like(int r);  // definido en las acciones

class KickTable : public Fl_Table_Row {
public:
    int handle(int e) override {
        if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
            int R = -1, C = -1;
            ResizeFlag rf = (ResizeFlag)0;
            TableContext tc = cursor2rowcol(R, C, rf);
            if (tc == CONTEXT_CELL && C == 4 && R >= 0 &&
                R < (int)g_view.size()) {
                select_row(R);
                toggle_like(R);
                return 1;
            }
        }
        return Fl_Table_Row::handle(e);
    }

    KickTable(int X, int Y, int W, int H) : Fl_Table_Row(X, Y, W, H) {
        cols(5);
        col_header(1);
        col_resize(1);
        row_header(0);
        rows(0);
        type(SELECT_SINGLE);
        end();
    }

protected:
    void draw_cell(TableContext ctx, int R, int C, int X, int Y, int W,
                   int H) override {
        const Palette &P = pal();
        static const char *hdrs[] = {"CANAL", "TITULO DEL DIRECTO",
                                     "ESPECT.", "CATEGORIA", "LIKE"};
        switch (ctx) {
        case CONTEXT_STARTPAGE:
            fl_font(FL_HELVETICA, 13);
            return;
        case CONTEXT_COL_HEADER:
            fl_push_clip(X, Y, W, H);
            fl_draw_box(FL_FLAT_BOX, X, Y, W, H, P.header);
            fl_color(P.txt);
            fl_font(FL_HELVETICA_BOLD, 12);
            fl_draw(hdrs[C], X + 8, Y, W - 16, H, FL_ALIGN_LEFT);
            fl_pop_clip();
            return;
        case CONTEXT_CELL: {
            fl_push_clip(X, Y, W, H);
            bool sel = row_selected(R) != 0;
            Fl_Color bg = sel ? P.sel : ((R % 2) ? P.row_alt : P.row);
            fl_draw_box(FL_FLAT_BOX, X, Y, W, H, bg);
            if (R < 0 || R >= (int)g_view.size()) { fl_pop_clip(); return; }
            const Row &r = g_view[R];
            fl_color(sel ? P.sel_txt : P.txt);
            if (C == 0) {
                // punto «en vivo» lila neon + nombre del canal
                fl_color(live_dot_color(sel));
                fl_begin_polygon();
                fl_circle(X + 10, Y + H / 2, 4.5);
                fl_end_polygon();
                fl_color(sel ? P.sel_txt : P.txt);
                fl_font(FL_HELVETICA_BOLD, 13);
                fl_draw(r.user.c_str(), X + 20, Y, W - 24, H, FL_ALIGN_LEFT);
            } else if (C == 1) {
                fl_font(FL_HELVETICA, 13);
                fl_draw(r.title.c_str(), X + 8, Y, W - 16, H, FL_ALIGN_LEFT);
            } else if (C == 2) {
                fl_font(FL_HELVETICA, 13);
                std::string v = fmt_viewers(r.viewers);
                fl_draw(v.c_str(), X + 4, Y, W - 12, H, FL_ALIGN_RIGHT);
            } else if (C == 3) {
                fl_font(FL_HELVETICA, 13);
                fl_color(sel ? P.sel_txt : P.txt);
                fl_draw(r.category.c_str(), X + 8, Y, W - 16, H,
                        FL_ALIGN_LEFT);
            } else {
                draw_thumb(X, Y, W, H, g_likes.count(r.slug) != 0);
            }
            fl_pop_clip();
            return;
        }
        default:
            return;
        }
    }
};

static KickTable *g_table = nullptr;

static int selected_row() {
    for (int r = 0; r < (int)g_view.size(); r++)
        if (g_table->row_selected(r)) return r;
    return -1;
}

// ---------------------------------------------------------------- acciones
static void apply_filter(bool announce);
static void update_favs_btn();

static void toggle_like(int r) {
    if (r < 0 || r >= (int)g_view.size()) return;
    const std::string slug = g_view[r].slug;
    const std::string user = g_view[r].user;
    if (g_likes.count(slug)) {
        g_likes.erase(slug);
        set_status("Like quitado de " + user + ".");
    } else {
        g_likes.insert(slug);
        set_status("Me gusta: " + user +
                   " (pulgar lila, canal en Favoritos).");
    }
    cfg_save();
    if (g_favs_mode) apply_filter(false);
    g_table->redraw();
}

static void update_favs_btn() {
    if (!g_btn_favs) return;
    if (g_favs_mode) {
        g_btn_favs->color(tw_purple());
        g_btn_favs->labelcolor(fl_rgb_color(255, 255, 255));
    } else {
        const Palette &P = pal();
        g_btn_favs->color(P.btn);
        g_btn_favs->labelcolor(P.txt);
    }
    g_btn_favs->box(FL_BORDER_BOX);
    g_lbl_head->copy_label(g_favs_mode
                               ? "Favoritos - solo tus canales con like"
                               : "En vivo ahora - sin cuenta (via anonima)");
    g_btn_favs->redraw();
}

static void favs_cb(Fl_Widget *, void *) {
    g_favs_mode = !g_favs_mode;
    update_favs_btn();
    apply_filter(false);
    if (g_favs_mode) {
        char m[128];
        snprintf(m, sizeof(m), "Favoritos: %zu canales con like.",
                 g_view.size());
        set_status(m);
    } else {
        set_status("Directos: lista completa.");
    }
}

static void busy(bool b) {
    if (b) {
        g_btn_search->deactivate();
        g_btn_list->deactivate();
        g_btn_favs->deactivate();
        g_btn_play->deactivate();
    } else {
        g_btn_search->activate();
        g_btn_list->activate();
        g_btn_favs->activate();
        g_btn_play->activate();
    }
}

static void run_job(JobKind kind, const std::string &q, const char *msg,
                    bool silent) {
    if (!silent) {
        busy(true);
        set_status(msg);
    }
    JobArg *a = new JobArg();
    a->kind = kind;
    a->query = q;
    pthread_t th;
    if (pthread_create(&th, nullptr, job_thread, a) == 0) {
        pthread_detach(th);
    } else {
        delete a;
        busy(false);
        set_status("No se pudo crear el hilo de red.");
    }
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static std::string slugify(std::string s) {
    s = lower_of(s);
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == ' ' || c == '-') c = '_';  // logins: [a-z0-9_]
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            out += c;
    }
    return out;
}

static void sugg_clear() {
    g_sugg_rows.clear();
    g_sugg_for.clear();
    g_sugg_inflight.clear();
    g_sugg_queued.clear();
}

static void do_list() {
    sugg_clear();
    run_job(JOB_LIST, "", "Cargando directos...");
}

static void do_search() {
    std::string q = trim(g_input->value());
    if (q.empty()) { do_list(); return; }
    sugg_clear();
    run_job(JOB_CHANNEL, slugify(q), "Buscando canal...");
}

static void do_play_row(int r) {
    if (r < 0 || r >= (int)g_view.size()) {
        set_status("Elige un directo de la lista.");
        return;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "Obteniendo directo de %s...",
             g_view[r].user.c_str());
    run_job(JOB_PLAY, g_view[r].slug, msg);
}

static void play_cb(Fl_Widget *, void *) { do_play_row(selected_row()); }
static void stop_cb(Fl_Widget *, void *) { player_stop(false); }
static void search_cb(Fl_Widget *, void *) { do_search(); }
static void list_cb(Fl_Widget *, void *) {
    if (g_favs_mode) {  // Directos saca de Favoritos al instante
        g_favs_mode = false;
        update_favs_btn();
        apply_filter(false);
        set_status("Directos: lista completa.");
        return;
    }
    do_list();
}

static void table_cb(Fl_Widget *, void *) {
    if (g_table->callback_context() != Fl_Table::CONTEXT_TABLE) return;
    int R = g_table->callback_row();
    if (R >= 0 && Fl::event_clicks() > 0) do_play_row(R);
}

// ------------------------------------------------------- filtro al teclear
// Al escribir en «Canal o categoría» la tabla se filtra AL MOMENTO y en
// local (sin red) sobre los directos ya cargados: coinciden canal o
// titulo; primero los que empiezan por lo escrito y luego los que solo
// lo contienen, conservando el orden por espectadores (el de g_rows)
// dentro de cada grupo. Detras van las sugerencias EN VIVO de la
// plataforma para ese mismo texto (si ya llegaron), sin duplicar y
// pasando tambien por el filtro de Favoritos si el modo esta activo.
// Al borrar el texto vuelve la lista completa. Enter/Buscar comprueba
// por API un canal concreto.
static bool g_has_filter = false;

static void apply_filter(bool announce) {
    std::string typed = trim(g_input->value());
    std::string q = lower_of(typed);
    // En modo Favoritos la base son solo los canales con like.
    std::vector<Row> base;
    for (size_t i = 0; i < g_rows.size(); i++)
        if (!g_favs_mode || g_likes.count(g_rows[i].slug))
            base.push_back(g_rows[i]);
    g_view.clear();
    size_t sugg_added = 0;
    if (q.empty()) {
        g_view = base;
    } else {
        std::set<std::string> shown;
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < base.size(); i++) {
                std::string u = lower_of(base[i].user);
                std::string t = lower_of(base[i].title);
                bool starts = u.compare(0, q.size(), q) == 0 ||
                              t.compare(0, q.size(), q) == 0;
                bool has = u.find(q) != std::string::npos ||
                           t.find(q) != std::string::npos;
                if ((pass == 0 && starts) || (pass == 1 && has && !starts)) {
                    g_view.push_back(base[i]);
                    shown.insert(base[i].slug);
                }
            }
        // Sugerencias EN VIVO de la plataforma para este mismo texto,
        // ya ordenadas por seguidores en el servidor. Sin duplicar con
        // los locales; en modo Favoritos solo entran las que tienen like.
        if (g_sugg_for == q)
            for (size_t i = 0; i < g_sugg_rows.size(); i++) {
                const Row &sr = g_sugg_rows[i];
                if (shown.count(sr.slug)) continue;
                if (g_favs_mode && !g_likes.count(sr.slug)) continue;
                g_view.push_back(sr);
                shown.insert(sr.slug);
                sugg_added++;
            }
    }
    g_table->rows((int)g_view.size());
    if (!g_view.empty()) g_table->select_row(0);
    g_table->redraw();
    if (announce && g_player_pid <= 0) {
        char m[224];
        if (!q.empty()) {
            if (sugg_added > 0)
                snprintf(m, sizeof(m),
                         "Filtro local: %zu de %zu directos y %zu "
                         "sugeridos en vivo para \xc2\xab%s\xc2\xbb.",
                         g_view.size() - sugg_added, base.size(),
                         sugg_added, typed.c_str());
            else
                snprintf(m, sizeof(m),
                         "Filtro local: %zu de %zu directos para \xc2\xab%s\xc2\xbb.",
                         g_view.size(), base.size(), typed.c_str());
            set_status(m);
        } else if (g_has_filter) {
            snprintf(m, sizeof(m),
                     "Filtro quitado: %zu directos en vivo.", g_rows.size());
            set_status(m);
        }
    }
    g_has_filter = !q.empty();
}

// El Enter NO se decide en el callback de texto: con FL_WHEN_CHANGED el
// event_key() no discrimina de forma fiable y el Enter se lo tragaba
// FLTK (bug firmado: el usuario se quedaba en «Filtro local: 0 de N»
// aunque el canal estuviera en vivo, p. ej. locosporlinux a las 18:16
// CDT con 19 espectadores por api/v2). SearchInput caza el FL_KEYDOWN
// antes de que la tecla se pierda y llama a do_search() directamente.
static void do_search();
class SearchInput : public Fl_Input {
public:
    SearchInput(int X, int Y, int W, int H) : Fl_Input(X, Y, W, H) {}
    int handle(int e) override {
        if (e == FL_KEYDOWN) {
            int k = Fl::event_key();
            if (k == FL_Enter || k == FL_KP_Enter) {
                do_search();
                return 1;
            }
        }
        return Fl_Input::handle(e);
    }
};

static void input_changed_cb(Fl_Widget *, void *) {
    apply_filter(true);
    // Debounce de sugerencias EN VIVO: watch_cb dispara JOB_SUGGEST a
    // los ~0,5 s del último tecleo (>= 2 caracteres). Con menos, o al
    // borrar, las sugerencias se retiran.
    std::string q = lower_of(trim(g_input->value()));
    if (q.size() >= 2) {
        g_sugg_queued = q;
        g_last_key_t = now_s();
    } else {
        g_sugg_queued.clear();
        g_sugg_for.clear();
        g_sugg_rows.clear();
    }
}

static void quality_cb(Fl_Widget *, void *) {
    const int NQ = (int)(sizeof(QUALITIES) / sizeof(QUALITIES[0]));
    int idx = 0;
    for (int i = 0; i < NQ; i++) if (QUALITIES[i] == g_quality) idx = i;
    g_quality = QUALITIES[(idx + 1) % NQ];
    static char buf[32];
    snprintf(buf, sizeof(buf), "Calidad: %dp", g_quality);
    g_btn_quality->label(buf);
    cfg_save();
    char m[96];
    snprintf(m, sizeof(m), "Calidad fijada en %dp (se aplica al reproducir).",
             g_quality);
    set_status(m);
}

// ---------------------------------------------------------------- tema UI
static void apply_theme() {
    const Palette &P = pal();
    g_win->color(P.win);
    g_lbl_search->labelcolor(P.txt);
    g_lbl_head->labelcolor(P.txt);
    g_lbl_player->labelcolor(P.txt);
    g_status->labelcolor(P.txt);
    g_version->labelcolor(P.txt);
    g_input->color(P.input_bg);
    g_input->textcolor(P.txt);
    g_input->cursor_color(P.txt);
    g_input->selection_color(P.sel);
    Fl_Button *btns[] = {g_btn_search, g_btn_list, g_btn_theme,
                         g_btn_quality, g_btn_stop, g_btn_info};
    for (int i = 0; i < 6; i++) {
        btns[i]->color(P.btn);
        btns[i]->labelcolor(P.txt);
        btns[i]->box(FL_BORDER_BOX);
    }
    update_favs_btn();
    g_btn_play->color(tw_purple());
    g_btn_play->labelcolor(fl_rgb_color(255, 255, 255));
    g_btn_theme->label(g_dark ? "Modo claro" : "Modo oscuro");
    g_table->color(P.win);
    g_win->redraw();
    g_table->redraw();
}

static void theme_cb(Fl_Widget *, void *) {
    g_dark = !g_dark;
    cfg_save();
    apply_theme();
    set_status(g_dark ? "Modo oscuro activado." : "Modo claro activado.");
}

static void awake_cb(void *p) {
    JobResult *res = (JobResult *)p;
    if (res->kind != JOB_SUGGEST) busy(false);
    if (res->kind == JOB_SUGGEST) {
        // Silencioso: solo vale si responde al texto que sigue escrito.
        g_sugg_inflight.clear();
        if (res->ok && res->query == lower_of(trim(g_input->value()))) {
            g_sugg_rows = res->rows;
            g_sugg_for = res->query;
            apply_filter(true);  // anuncia «… y N sugeridos en vivo»
        }
        delete res;
        return;
    }
    if (res->kind == JOB_PLAY) {
        if (res->ok)
            player_launch(res->play_url, res->play_label, res->via_master);
        else set_status(res->err);
        delete res;
        return;
    }
    if (res->ok) {
        g_rows = res->rows;
        apply_filter(false);   // respeta el texto que haya en el buscador
        if (!res->note.empty()) set_status(res->note);
        else if (g_rows.empty()) set_status("Sin directos que mostrar.");
    } else {
        if (res->kind == JOB_CHANNEL && !res->note.empty()) {
            // canal existe pero fuera de directo: note trae el usuario
            set_status(std::string("\xc2\xab") + res->note +
                       "\xc2\xbb no esta en directo ahora.");
        } else if (res->kind == JOB_CHANNEL && res->notfound) {
            // no existe como canal: intentar como categoría (juego)
            std::string q = trim(g_input->value());
            delete res;
            run_job(JOB_CATEGORY, q, "Buscando como categoria...");
            return;
        } else {
            set_status(res->err);
        }
    }
    delete res;
}

// ---------------------------------------------------------------- ayuda
// Ventana «Ayuda de FLTwitcher» (estilo FLTube, aprobada por Juanca en
// maqueta v4): cabecera con los prismaticos, tres pestanas como botones (activa
// en el lila neon del logo con etiqueta oscura; las inactivas
// del color del cuadro de texto, para fundirse con el) y pie con el
// aviso de cliente no oficial. Sin apartado de Atajos. Se repinta con
// la paleta actual de la app: si FLTwitcher esta en modo claro, la ayuda
// sale en claro (y al reves), llamando a help_theme() al abrir.
static Fl_Window *g_help_win = nullptr;
static Fl_Button *g_help_tabs[3];
static Fl_Group *g_help_pages[3];
static Fl_Text_Display *g_help_txt[3];
static Fl_Box *g_help_tocolor[16];  // etiquetas que siguen a P.txt
static Fl_Box *g_help_dim[4];       // etiquetas atenuadas (version, etc.)
static Fl_Box *g_help_url = nullptr;

static Fl_Color logo_green() { return fl_rgb_color(169, 112, 255); }  // lila neon
static Fl_Color dim_color() {
    return g_dark ? fl_rgb_color(148, 154, 166) : fl_rgb_color(105, 105, 105);
}

static void help_tab_cb(Fl_Widget *, void *v) {
    const Palette &P = pal();
    int sel = (int)(long)v;
    for (int i = 0; i < 3; i++) {
        if (i == sel) {
            g_help_tabs[i]->color(logo_green());
            g_help_tabs[i]->labelcolor(fl_rgb_color(25, 8, 45));
            g_help_pages[i]->show();
        } else {
            g_help_tabs[i]->color(P.input_bg);
            g_help_tabs[i]->labelcolor(P.txt);
            g_help_pages[i]->hide();
        }
        g_help_tabs[i]->redraw();
    }
}

static void help_theme() {
    const Palette &P = pal();
    g_help_win->color(P.win);
    int sel = 0;
    for (int i = 0; i < 3; i++) {
        g_help_pages[i]->color(P.win);
        g_help_txt[i]->color(P.input_bg);
        g_help_txt[i]->textcolor(P.txt);
        if (g_help_pages[i]->visible()) sel = i;
    }
    help_tab_cb(nullptr, (void *)(long)sel);
    for (int i = 0; g_help_tocolor[i]; i++)
        g_help_tocolor[i]->labelcolor(P.txt);
    for (int i = 0; g_help_dim[i]; i++)
        g_help_dim[i]->labelcolor(dim_color());
    if (g_help_url)
        g_help_url->labelcolor(g_dark ? tw_lilac()
                                      : fl_rgb_color(115, 45, 205));
    g_help_win->redraw();
}

static Fl_Text_Display *help_mktxt(int x, int y, int w, int h,
                                   const char *txt) {
    Fl_Text_Display *d = new Fl_Text_Display(x, y, w, h);
    d->box(FL_DOWN_BOX);
    d->textfont(FL_HELVETICA);
    d->textsize(13);
    d->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    Fl_Text_Buffer *b = new Fl_Text_Buffer();
    b->text(txt);
    d->buffer(b);
    return d;
}

static Fl_Box *help_box(int x, int y, int w, int h, const char *t,
                        Fl_Box **to_color, Fl_Box **dim) {
    Fl_Box *b = new Fl_Box(x, y, w, h, t);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    b->labelsize(13);
    if (to_color) *to_color = b;
    if (dim) *dim = b;
    return b;
}

static void open_help() {
    if (!g_help_win) {
        int tc = 0, dm = 0;
        g_help_win = new Fl_Window(640, 560, "Ayuda de FLTwitcher");

        static Fl_Image *hlogo = nullptr;
        {
            static const char *paths[] = {
                "icons/fltwitcher-128.png",
                "/usr/local/share/pixmaps/fltwitcher-128.png",
                "/usr/local/share/pixmaps/fltwitcher.png",
                "icons/fltwitcher-512.png",
                "icons/fltwitcher-48.png",
                "fltwitcher.png",
                nullptr,
            };
            for (int i = 0; paths[i] && !hlogo; i++) {
                Fl_PNG_Image *raw = new Fl_PNG_Image(paths[i]);
                if (raw->w() > 0 && raw->h() > 0)
                    hlogo = raw->copy(112, 112);
                delete raw;
            }
        }
        Fl_Box *blogo = new Fl_Box(18, 18, 112, 112);
        if (hlogo) blogo->image(hlogo);
        blogo->box(FL_NO_BOX);

        const int TX = 152;
        Fl_Box *name = help_box(TX, 14, 470, 34, "FLTwitcher",
                                &g_help_tocolor[tc++], nullptr);
        name->labelfont(FL_HELVETICA_BOLD);
        name->labelsize(26);
        help_box(TX, 54, 470, 20, "versi\xc3\xb3n 0.1", nullptr,
                 &g_help_dim[dm++]);
        help_box(TX, 78, 470, 20,
                 "Mira directos de Twitch en tu antiguo PC, sin cuenta.",
                 &g_help_tocolor[tc++], nullptr);
        help_box(TX, 106, 108, 20, "Tecnolog\xc3\xad" "as:",
                 &g_help_tocolor[tc++], nullptr);
        static const char *techs[] = {"FLTK", "curl", "ffmpeg/mplayer",
                                      "C++", nullptr};
        static const int techx[] = {272, 340, 402, 552};
        for (int i = 0; techs[i]; i++)
            help_box(techx[i], 106, 90, 20, techs[i],
                     &g_help_tocolor[tc++], nullptr);
        help_box(TX, 130, 118, 20, "Licencia:",
                 &g_help_tocolor[tc++], nullptr);
        Fl_Box *url = help_box(272, 130, 340, 20,
                               "GNU GPL v3.0",
                               nullptr, nullptr);
        url->labelfont(FL_HELVETICA_BOLD);
        url->labelcolor(tw_lilac());
        g_help_url = url;
        help_box(TX, 154, 470, 20, "Para PC antiguos: Tiny Core / FLinux", nullptr,
                 &g_help_dim[dm++]);
        g_help_tocolor[tc] = nullptr;
        g_help_dim[dm] = nullptr;

        static const char *tabnames[3] = {
            "C\xc3\xb3mo se usa", "Configuraci\xc3\xb3n", "Autores"};
        static const int tabx[3] = {10, 198, 386};
        static const int tabw[3] = {180, 180, 140};
        for (int i = 0; i < 3; i++) {
            Fl_Button *b = new Fl_Button(tabx[i], 186, tabw[i], 32,
                                         tabnames[i]);
            b->box(FL_UP_BOX);
            b->labelfont(FL_HELVETICA_BOLD);
            b->labelsize(13);
            b->callback(help_tab_cb, (void *)(long)i);
            g_help_tabs[i] = b;
        }

        static const char *txts[3] = {
            "FLTwitcher es una aplicaci\xc3\xb3n para ver directos de "
            "Twitch sin cuenta. Muestra primero los directos con m\xc3\xa1s "
            "espectadores y, al escribir en \xc2\xab" "Canal o "
            "categor\xc3\xad" "a\xc2\xab, filtra esa lista al momento.\n"
            "\n"
            "Mientras escribes, FLTwitcher tambi\xc3\xa9n busca canales en "
            "directo en toda la plataforma y te los sugiere en la misma "
            "lista. Da Enter (o \xc2\xab" "Buscar\xc2\xbb) para comprobar "
            "canal concreto: si est\xc3\xa1 en directo aparece su fila; "
            "si no, te lo dice.\n"
            "\n"
            "Pulsa \xc2\xabREPRODUCIR\xc2\xbb (o doble clic sobre la "
            "fila) para abrir el v\xc3\xad" "deo. La calidad va de 160p a "
            "720p, con 360p por defecto.\n"
            "\n"
            "Pulsa el pulgar de un canal para marcarlo como favorito: "
            "queda guardado, y en \xc2\xab" "Favoritos\xc2\xbb la lista se "
            "filtra a tus canales con like que est\xc3\xa1n en directo.",
            "La configuraci\xc3\xb3n se guarda sola en ~/.fltwitcherrc: el "
            "modo oscuro o claro, la calidad por defecto (360p) y tus "
            "favoritos (los likes).\n"
            "\n"
            "Todo sobrevive a cerrar la aplicaci\xc3\xb3n y a reiniciar "
            "el equipo.\n"
            "\n"
            "Sin cuentas ni cookies: FLTwitcher no guarda credenciales de "
            "Twitch; todo funciona de forma an\xc3\xb3nima, estilo "
            "FreeTube.",
            "FLTwitcher \xc2\xa9 Juan Carlos\n"
            "\n"
            "Proyectos que inspiraron FLTwitcher: FLTube, de Facundo "
            "Adorno; FLTV, de AnddyCort; y FLinux, de Nicol\xc3\xa1s "
            "Lombardi, creador de FLinux.\n"
            "\n"
            "Cr\xc3\xa9" "ditos: a la comunidad FLTK y a Tiny Core "
            "Linux.\n"
            "\n"
            "Cliente no oficial, sin afiliaci\xc3\xb3n con Twitch.\n"
            "\n"
            "Licenciado bajo GNU GPL v3.0."};

        for (int i = 0; i < 3; i++) {
            g_help_pages[i] = new Fl_Group(10, 218, 620, 264, "");
            g_help_txt[i] = help_mktxt(20, 228, 600, 244, txts[i]);
            g_help_pages[i]->end();
        }
        g_help_pages[1]->hide();
        g_help_pages[2]->hide();

        help_box(14, 524, 470, 20,
                 "Cliente no oficial, sin afiliaci\xc3\xb3n con Twitch.",
                 nullptr, &g_help_dim[2]);
        g_help_dim[3] = nullptr;
        Fl_Box *gpl = new Fl_Box(500, 524, 126, 20, "GPL-3.0");
        gpl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        gpl->labelsize(12);
        gpl->labelcolor(dim_color());

        g_help_win->end();
    }
    help_theme();
    g_help_win->show();
}

static void info_cb(Fl_Widget *, void *) { open_help(); }

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    cfg_load();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Fl::lock();
    Fl::scheme("gtk+");

    g_win = new Fl_Window(920, 600, "FLTwitcher - Twitch en vivo");
    g_win->color(pal().win);

    // Logo de prismaticos de FLTwitcher en la esquina superior
    // izquierda, como marca junto a la fila del buscador. Se carga
    // el PNG del paquete (fltwitcher-48.png instalado en pixmaps);
    // ruta de reserva para ejecutar desde el arbol de desarrollo.
    // Es el mismo PNG en modo oscuro y claro.
    static Fl_Image *s_logo = nullptr;
    {
        static const char *paths[] = {
            "/usr/local/share/pixmaps/fltwitcher.png",
            "icons/fltwitcher-48.png",
            "fltwitcher.png",
            nullptr,
        };
        for (int i = 0; paths[i] && !s_logo; i++) {
            Fl_PNG_Image *raw = new Fl_PNG_Image(paths[i]);
            if (raw->w() > 0 && raw->h() > 0)
                s_logo = raw->copy(42, 42);
            delete raw;
        }
        Fl_Box *logo = new Fl_Box(10, 4, 44, 44);
        if (s_logo) logo->image(s_logo);
        logo->box(FL_NO_BOX);
    }

    g_lbl_search = new Fl_Box(60, 12, 130, 26, "Canal o categoria:");
    g_lbl_search->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    g_lbl_search->labelsize(13);

    g_input = new SearchInput(195, 12, 290, 26);
    g_input->textsize(13);
    g_input->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY);
    g_input->callback(input_changed_cb);
    g_input->tooltip("Escribe y la tabla se filtra al momento y te "
                     "sugiere canales en directo; Enter comprueba el "
                     "canal en Twitch.");


    g_btn_info = new Fl_Button(862, 12, 48, 26, "i");
    g_btn_info->labelfont(FL_HELVETICA_BOLD | FL_ITALIC);
    g_btn_info->labelsize(16);
    g_btn_info->callback(info_cb);
    g_btn_info->tooltip("Ayuda de FLTwitcher");

    g_btn_search = new Fl_Button(493, 12, 100, 26, "Buscar");
    g_btn_search->callback(search_cb);
    g_btn_list = new Fl_Button(601, 12, 120, 26, "Directos");
    g_btn_list->callback(list_cb);
    g_btn_favs = new Fl_Button(729, 12, 120, 26, "Favoritos");
    g_btn_favs->callback(favs_cb);

    g_lbl_head = new Fl_Box(10, 48, 480, 24,
                            "En vivo ahora - sin cuenta (via anonima)");
    g_lbl_head->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    g_lbl_head->labelfont(FL_HELVETICA_BOLD);
    g_lbl_head->labelsize(13);

    g_btn_theme = new Fl_Button(700, 48, 210, 26, "Modo claro");
    g_btn_theme->callback(theme_cb);

    g_table = new KickTable(10, 80, 900, 418);
    g_table->col_width(0, 182);
    g_table->col_width(1, 352);
    g_table->col_width(2, 84);
    g_table->col_width(3, 168);
    g_table->col_width(4, 84);
    g_table->row_height_all(26);
    g_table->col_header_height(26);
    g_table->callback(table_cb);
    g_table->when(FL_WHEN_RELEASE);

    g_btn_play = new Fl_Button(10, 510, 250, 40, "REPRODUCIR");
    g_btn_play->labelfont(FL_HELVETICA_BOLD);
    g_btn_play->labelsize(15);
    g_btn_play->color(tw_purple());
    g_btn_play->callback(play_cb);

    g_btn_quality = new Fl_Button(272, 510, 140, 40);
    {
        static char qb[32];
        snprintf(qb, sizeof(qb), "Calidad: %dp", g_quality);
        g_btn_quality->label(qb);
    }
    g_btn_quality->callback(quality_cb);

    g_btn_stop = new Fl_Button(424, 510, 130, 40, "Detener");
    g_btn_stop->callback(stop_cb);

    g_lbl_player = new Fl_Box(566, 518, 200, 24, "Reproductor: mplayer");
    g_lbl_player->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    g_lbl_player->labelsize(12);

    g_status = new Fl_Box(10, 566, 690, 22,
                          "Listo - via anonima estilo FreeTube - sin cuenta de Twitch");
    g_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    g_status->labelsize(12);

    g_version = new Fl_Box(710, 566, 200, 22, "FLTwitcher 0.1 - FLTK - C++");
    g_version->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    g_version->labelsize(12);

    g_win->resizable(g_table);
    g_win->end();
    apply_theme();
    g_win->show();

    Fl::add_timeout(0.5, watch_cb);
    do_list();
    return Fl::run();
}
