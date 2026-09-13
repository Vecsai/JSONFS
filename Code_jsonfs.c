/*
 * JSON FS — a simple FUSE-based filesystem that lets you work with a JSON file as a regular directory tree.
 * Copyright (c) 2026 Vecsai
 * Distributed under the MIT License. See LICENSE file for details.
 * Project: https://github.com/Vecsai/JSONFS
 */

#define FUSE_USE_VERSION 30 // Версия API FUSE 3.0 для совместимости с библиотекой

// Подключение стандартных и сторонних библиотек
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fuse.h>
#include <jansson.h>
#include <errno.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/file.h>
#include <syslog.h>
#include <signal.h>

// Глобальные переменные для корня JSON и пути к JSON-файлу
json_t *root;
char *json_file;


// -----------------------------
// Загрузка JSON-файла из диска
// -----------------------------
void load_json() {
    json_error_t error;
    root = json_load_file(json_file, 0, &error);
    if (!root) {
        fprintf(stderr, "Error loading JSON file: %s\n", error.text);
        root = json_object();
    }
}

// ----------------------------------------
// Рекурсивная очистка JSON-объекта от ключей,
// начинающихся с точки (скрытые файлы/папки)
// ----------------------------------------
void clean_json(json_t *node) {
    if (!json_is_object(node)) return;

    const char *key;
    json_t *value;

    // Итерация по ключам объекта
    void *iter = json_object_iter(node);
    while (iter) {
        key = json_object_iter_key(iter);
        value = json_object_iter_value(iter);

        // Удаление ключей, начинающихся с точки
        if (key && key[0] == '.') {
            iter = json_object_iter_next(node, iter); // Переход к следующему итератору перед удалением
            json_object_del(node, key);
        } else {
            iter = json_object_iter_next(node, iter);
        }
    }
}

// -----------------------------
// Сохранение JSON-объекта в файл
// -----------------------------
void save_json() {
    // Очистка от скрытых элементов перед сохранением
    clean_json(root);

    if (json_dump_file(root, json_file, JSON_INDENT(4)) != 0) {
        fprintf(stderr, "Error saving JSON file\n");
    }
}

// -----------------------------
// Логирование операций в syslog
// -----------------------------
void log_operation(const char *op, const char *path) {
    openlog("jsonfs", LOG_PID|LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "[%s] %s", op, path);
    closelog();
}

// ------------------------------------
// Инициализация файловой системы FUSE
// ------------------------------------
void* jsonfs_init(struct fuse_conn_info *conn) {
    // Проверка доступа к JSON-файлу
    if(access(json_file, R_OK|W_OK) != 0) {
        syslog(LOG_ERR, "JSON file access denied: %s", strerror(errno));
        return NULL;
    }
    
    // Загрузка JSON при монтировании
    load_json();
    if(!root) {
        syslog(LOG_ERR, "Failed to initialize JSON root");
        return NULL;
    }
    
    return NULL;
}

// ------------------------------------
// Очистка ресурсов при размонтировании
// ------------------------------------
void jsonfs_destroy(void *private_data) {
    if(root) {
        json_decref(root);
        root = NULL;
    }
}

// -----------------------------
// Обработчик сигналов (SIGINT, SIGTERM)
// для сохранения данных и корректного завершения
// -----------------------------
void sig_handler(int signo) {
    if(signo == SIGINT || signo == SIGTERM) {
        save_json();
        sync();
        exit(0);
    }
}

// --------------------------------------------------
// Получение JSON-объекта по файловому пути ("/a/b/c")
// --------------------------------------------------
json_t *get_json_by_path(const char *path) {
    if(!path || strcmp(path, "") == 0) return root;
    
    // Убираем ведущий слэш
    if(path[0] == '/') path++;
    
    char *path_copy = strdup(path);
    char *saveptr = NULL;
    char *token = strtok_r(path_copy, "/", &saveptr);
    json_t *current = root;

    while(token) {
        if(!json_is_object(current)) {
            free(path_copy);
            return NULL;
        }
        
        current = json_object_get(current, token);
        if(!current) {
            free(path_copy);
            return NULL;
        }
        
        token = strtok_r(NULL, "/", &saveptr);
    }

    free(path_copy);
    return current;
}

// --------------------------------------------
// Получение атрибутов файла или каталога (stat)
// --------------------------------------------
int jsonfs_getattr(const char *path, struct stat *stbuf) {
    log_operation("GETATTR", path);
    memset(stbuf, 0, sizeof(struct stat));
    
    // Обработка корневого каталога
    if(strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }

    // Убираем ведущий слэш
    const char *rel_path = path[0] == '/' ? path + 1 : path;
    
    // Поиск JSON-объекта по пути
    char *path_copy = strdup(rel_path);
    char *saveptr = NULL;
    char *token = strtok_r(path_copy, "/", &saveptr);
    json_t *current = root;
    
    while(token) {
        if(!json_is_object(current)) {
            free(path_copy);
            return -ENOENT;
        }
        
        current = json_object_get(current, token);
        if(!current) {
            free(path_copy);
            return -ENOENT;
        }
        
        token = strtok_r(NULL, "/", &saveptr);
    }
    
    free(path_copy);

    // Определение типа объекта и заполнение атрибутов
    if(json_is_object(current)) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        
        if(json_is_string(current)) {
            stbuf->st_size = strlen(json_string_value(current));
        }
        else if(json_is_integer(current)) {
            stbuf->st_size = snprintf(NULL, 0, "%ld", json_integer_value(current));
        }
        else if(json_is_real(current)) {
            stbuf->st_size = snprintf(NULL, 0, "%.6f", json_real_value(current));
        }
        else if(json_is_boolean(current)) {
            stbuf->st_size = json_is_true(current) ? 4 : 5;
        }
        else if(json_is_null(current)) {
            stbuf->st_size = 4;
        }
    }

    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = time(NULL);
    
    return 0;
}

// ------------------------------------
// Чтение содержимого каталога (readdir)
// ------------------------------------
int jsonfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, 
                  off_t offset, struct fuse_file_info *fi) {
    log_operation("READDIR", path);
    
    // Добавляем стандартные записи
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    json_t *dir = get_json_by_path(path + 1);
    if(!dir || !json_is_object(dir)) return -ENOENT;

    const char *key;
    json_t *value;
    json_object_foreach(dir, key, value) {
        // Пропускаем скрытые файлы/каталоги (начинающиеся с '.')
        if(key[0] != '.') {
            if(filler(buf, key, NULL, 0) != 0) break;
        }
    }

    return 0;
}

// -----------------------------
// Создание каталога (mkdir)
// -----------------------------
int jsonfs_mkdir(const char *path, mode_t mode) {
    log_operation("MKDIR", path);
    
    char *path_copy = strdup(path);
    if (!path_copy) return -ENOMEM;
    
    char *current_path = strdup("");
    if (!current_path) {
        free(path_copy);
        return -ENOMEM;
    }
    
    // Убираем ведущий слэш
    char *p = path_copy;
    if (p[0] == '/') p++;
    
    char *saveptr = NULL;
    char *token = strtok_r(p, "/", &saveptr);
    
    json_t *current = root;
    
    // Проходим по пути и создаём недостающие каталоги
    while (token) {
        json_t *next = json_object_get(current, token);
        
        if (!next) {
            // Создаем новый объект (каталог)
            json_object_set_new(current, token, json_object());
            next = json_object_get(current, token);
        } else if (!json_is_object(next)) {
            // Если элемент не каталог, вернуть ошибку
            free(path_copy);
            free(current_path);
            return -ENOTDIR;
        }
        
        current = next;
        token = strtok_r(NULL, "/", &saveptr);
    }
    
    save_json();
    free(path_copy);
    free(current_path);
    return 0;
}

// -----------------------------
// Создание файла (create)
// -----------------------------
int jsonfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    log_operation("CREATE", path);
    
    char *path_copy = strdup(path);
    char *dir = dirname(path_copy);
    char *name = basename(strdup(path));

    // Запрет создания скрытых файлов
    if(name[0] == '.') {
        free(path_copy);
        return -EPERM; // Operation not permitted
    }

    // Автоматическое создание родительских каталогов
    if(strcmp(dir, "/") != 0) {
        int res = jsonfs_mkdir(dir, 0755);
        if(res != 0 && res != -EEXIST) {
            free(path_copy);
            return res;
        }
    }

    // Получаем родительский объект JSON
    json_t *parent = get_json_by_path(dir + 1);
    if(!parent || !json_is_object(parent)) {
        free(path_copy);
        return -ENOENT;
    }

    // Создаем пустую строку как содержимое нового файла
    json_object_set_new(parent, name, json_string(""));
    
    save_json();
    free(path_copy);
    return 0;
}

// -----------------------------
// Открытие файла (open)
// -----------------------------
int jsonfs_open(const char *path, struct fuse_file_info *fi) {
    log_operation("OPEN", path);
    
    // Получаем JSON-объект файла
    json_t *file = get_json_by_path(path + 1);
    if(!file) return -ENOENT;
    if(json_is_object(file)) return -EISDIR;
    
    // Для операций записи блокируем JSON-файл
    if(fi->flags & (O_WRONLY | O_RDWR)) {
        int fd = open(json_file, O_RDWR);
        if(fd < 0) {
            syslog(LOG_ERR, "Failed to open JSON file: %s", strerror(errno));
            return -errno;
        }
        
        if(flock(fd, LOCK_EX) == -1) {
            close(fd);
            syslog(LOG_ERR, "File lock failed: %s", strerror(errno));
            return -errno;
        }
        fi->fh = (uint64_t)fd;
    }
    return 0;
}

// -----------------------------
// Закрытие файла (release)
// -----------------------------
int jsonfs_release(const char *path, struct fuse_file_info *fi) {
    log_operation("RELEASE", path);
    
    if(fi->fh) {
        int fd = (int)fi->fh;
        save_json(); // Сохраняем изменения перед разблокировкой
        flock(fd, LOCK_UN);
        close(fd);
    }
    return 0;
}

// -----------------------------
// Чтение файла (read)
// -----------------------------
int jsonfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    log_operation("READ", path);
    
    json_t *file = get_json_by_path(path + 1);
    if(!file) return -ENOENT;

    // Подготовка содержимого файла к чтению в строковом виде
    char *content = NULL;
    size_t len = 0;
    
    if(json_is_string(file)) {
        len = strlen(json_string_value(file));
        content = strdup(json_string_value(file));
    }
    else if(json_is_integer(file)) {
        len = snprintf(NULL, 0, "%d", (int)json_integer_value(file));
        content = malloc(len + 1);
        snprintf(content, len+1, "%d", (int)json_integer_value(file));
    }
    else if(json_is_real(file)) {
        len = snprintf(NULL, 0, "%.6f", json_real_value(file));
        content = malloc(len + 1);
        snprintf(content, len+1, "%.6f", json_real_value(file));
    }
    else if(json_is_boolean(file)) {
        content = strdup(json_is_true(file) ? "true" : "false");
        len = strlen(content);
    }
    else if(json_is_null(file)) {
        content = strdup("null");
        len = 4;
    }
    else {
        return -EINVAL;
    }

    if(!content) return -ENOMEM;

    // Проверка смещения чтения
    if(offset >= len) {
        free(content);
        return 0;
    }

    // Копирование данных в буфер
    size_t bytes_to_copy = (size < len - offset) ? size : len - offset;
    memcpy(buf, content + offset, bytes_to_copy);
    
    free(content);
    return bytes_to_copy;
}

// -----------------------------
// Переименование файла или каталога (rename)
// -----------------------------
int jsonfs_rename(const char *from, const char *to) {
    log_operation("RENAME", from);
    
    // Разбор исходного пути
    char *from_copy = strdup(from);
    char *from_dir = dirname(from_copy);
    char *from_name = basename(strdup(from));
    
    // Разбор целевого пути
    char *to_copy = strdup(to);
    char *to_dir = dirname(to_copy);
    char *to_name = basename(strdup(to));

    // Создание целевой директории при необходимости
    if(strcmp(to_dir, "/") != 0) {
        int res = jsonfs_mkdir(to_dir, 0755);
        if(res != 0 && res != -EEXIST) {
            free(from_copy);
            free(to_copy);
            return res;
        }
    }

    // Получение родительских JSON-объектов
    json_t *from_parent = get_json_by_path(from_dir + 1);
    json_t *to_parent = get_json_by_path(to_dir + 1);
    
    if(!from_parent || !to_parent) {
        free(from_copy);
        free(to_copy);
        return -ENOENT;
    }
    
    // Получение значения для переименования
    json_t *value = json_object_get(from_parent, from_name);
    if(!value) {
        free(from_copy);
        free(to_copy);
        return -ENOENT;
    }
    
    // Копирование значения в новое место и удаление старого
    json_object_set_new(to_parent, to_name, json_deep_copy(value));
    json_object_del(from_parent, from_name);
    
    save_json();
    sync(); // Синхронизация с диском
    free(from_copy);
    free(to_copy);
    return 0;
}

// -----------------------------
// Запись в файл (write)
// -----------------------------
int jsonfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    log_operation("WRITE", path);

    // Разбор пути на директорию и имя файла
    char *path_copy = strdup(path);
    char *dir_copy = strdup(path);
    char *name_copy = strdup(path);
    if(!path_copy || !dir_copy || !name_copy) {
        free(path_copy); free(dir_copy); free(name_copy);
        return -ENOMEM;
    }
    char *dir = dirname(dir_copy);
    char *name = basename(name_copy);

    // Получение родительского JSON-объекта
    json_t *parent = get_json_by_path(dir + 1);
    if(!parent || !json_is_object(parent)) {
        free(path_copy); free(dir_copy); free(name_copy);
        return -ENOENT;
    }

    // Получение текущего содержимого файла
    json_t *file_obj = json_object_get(parent, name);
    const char *old_content = json_is_string(file_obj) ? json_string_value(file_obj) : "";

    // Формирование нового содержимого с учётом смещения offset
    size_t old_len = strlen(old_content);
    size_t new_len = offset + size > old_len ? offset + size : old_len;
    char *new_content = calloc(new_len + 1, 1);
    if(!new_content) {
        free(path_copy); free(dir_copy); free(name_copy);
        return -ENOMEM;
    }

    // Копируем старое содержимое
    memcpy(new_content, old_content, old_len);

    // Копируем новые данные на позицию offset
    memcpy(new_content + offset, buf, size);

    // Обновляем JSON-объект
    json_object_set_new(parent, name, json_string(new_content));

    free(new_content);
    free(path_copy); free(dir_copy); free(name_copy);

    save_json();
    return size;
}

// -----------------------------
// Удаление файла (unlink)
// -----------------------------
int jsonfs_unlink(const char *path) {
    log_operation("UNLINK", path);
    
    char *path_copy = strdup(path);
    char *dir = dirname(path_copy);
    char *name = basename(strdup(path));

    // Получение родительского объекта JSON
    json_t *parent = get_json_by_path(dir + 1);
    if(!parent || !json_is_object(parent)) {
        free(path_copy);
        return -ENOENT;
    }
    
    // Проверка существования файла
    if(!json_object_get(parent, name)) {
        free(path_copy);
        return -ENOENT;
    }
    
    // Удаление из JSON-объекта
    json_object_del(parent, name);
    save_json();
    sync(); // Синхронизация с диском
    free(path_copy);
    return 0;
}

// -----------------------------
// Удаление каталога (rmdir)
// -----------------------------
int jsonfs_rmdir(const char *path) {
    log_operation("RMDIR", path);
    
    // Получение JSON-объекта каталога
    json_t *dir = get_json_by_path(path + 1);
    if(!dir || !json_is_object(dir)) return -ENOTDIR;
    
    // Проверка, что каталог пустой
    if(json_object_size(dir) > 0) return -ENOTEMPTY;
    
    // Разбор пути на родительский каталог и имя
    char *path_copy = strdup(path);
    char *parent_path = dirname(path_copy);
    char *name = basename(strdup(path));
    
    // Получение родительского объекта JSON
    json_t *parent = get_json_by_path(parent_path[0] == '/' ? parent_path + 1 : parent_path);
    if(!parent || !json_is_object(parent)) {
        free(path_copy);
        return -ENOENT;
    }
    
    // Проверка существования каталога
    if(!json_object_get(parent, name)) {
        free(path_copy);
        return -ENOENT;
    }
    
    // Удаление каталога из JSON
    json_object_del(parent, name);
    save_json();
    
    free(path_copy);
    return 0;
}

// -----------------------------
// Обрезка файла (truncate)
// -----------------------------
int jsonfs_truncate(const char *path, off_t length) {
    log_operation("TRUNCATE", path);
    
    // Поддерживается только обрезка до нуля
    if(length != 0) return -EINVAL;

    char *path_copy = strdup(path);
    char *dir = dirname(path_copy);
    char *name = basename(path_copy);

    // Получение родительского объекта JSON
    json_t *parent = get_json_by_path(dir + 1);
    if(!parent || !json_is_object(parent)) {
        free(path_copy);
        return -ENOENT;
    }

    // Обновление содержимого файла на пустую строку
    json_object_set_new(parent, name, json_string(""));
    save_json();
    
    free(path_copy);
    return 0;
}

// -----------------------------
// Сброс буфера (flush)
// -----------------------------
int jsonfs_flush(const char *path, struct fuse_file_info *fi) {
    log_operation("FLUSH", path);
    save_json(); // Принудительное сохранение при flush
    return 0;
}

// -----------------------------
// Описание операций FUSE
// -----------------------------
static struct fuse_operations jsonfs_oper = {
    .getattr = jsonfs_getattr, // Получение атрибутов файла или каталога (аналог stat)
    .readdir = jsonfs_readdir, // Чтение содержимого каталога (список файлов и папок)
    .mkdir = jsonfs_mkdir, // Создание нового каталога
    .rmdir = jsonfs_rmdir, // Удаление каталога
    .create = jsonfs_create, // Создание нового файла
    .rename = jsonfs_rename, // Переименование файла или каталога
    .open = jsonfs_open, // Открытие файла (проверка доступа, блокировка для записи)
    .read = jsonfs_read, // Чтение данных из файла
    .write = jsonfs_write, // Запись данных в файл
    .unlink = jsonfs_unlink, // Удаление файла
    .release = jsonfs_release, // Закрытие файла (освобождение ресурсов, снятие блокировок)
    .truncate = jsonfs_truncate, // Обрезка файла (truncate), поддерживается только обрезка до нуля
    .create = jsonfs_create, // Повторное указание create (может быть избыточным, но для O_CREAT)
    .init = jsonfs_init, // Инициализация файловой системы при монтировании
    .destroy = jsonfs_destroy, // Очистка и освобождение ресурсов при размонтировании
    .flush = jsonfs_flush // Сброс буферов и принудительное сохранение данных (flush)
};

// -----------------------------
// Основная функция программы
// -----------------------------
int main(int argc, char *argv[]) {
    if(argc < 3) {
        fprintf(stderr, "Usage: %s <mountpoint> <json-file> [-o options]\n", argv[0]);
        return 1;
    }

    // Получаем абсолютный путь к JSON-файлу
    json_file = realpath(argv[2], NULL);
    if(!json_file) {
        perror("JSON file path error");
        return 1;
    }

    // Формируем аргументы для FUSE
    char *fuse_argv[] = {
        argv[0], 
        argv[1],
        "-o", "allow_other,default_permissions",
        "-o", "auto_unmount",
        "-f", // Foreground mode для диагностики
        NULL
    };
    
    // Предварительная загрузка JSON-файла
    load_json();
    if(!root) {
        fprintf(stderr, "Critical error: Failed to load JSON\n");
        return 1;
    }
    
    // Установка обработчиков сигналов для корректного завершения
    signal(SIGINT, sig_handler); 
    signal(SIGTERM, sig_handler);

		// Вычисление количества аргументов для FUSE (без завершающего NULL)
    int fuse_argc = sizeof(fuse_argv)/sizeof(fuse_argv[0])-1;
		// Запуск FUSE с заданными аргументами и операциями файловой системы
    return fuse_main(fuse_argc, fuse_argv, &jsonfs_oper, NULL);
}