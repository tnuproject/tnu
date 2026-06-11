#ifndef TNU_USER_H
#define TNU_USER_H

#include <tnu/types.h>

#define USER_NAME_MAX 31
#define USER_HOME_MAX 127
#define USER_SHELL_MAX 127
#define USER_HASH_SCHEME "tnu1"

struct user_record {
    char name[USER_NAME_MAX + 1];
    uint32_t uid;
    uint32_t gid;
    char home[USER_HOME_MAX + 1];
    char shell[USER_SHELL_MAX + 1];
    uint32_t salt;
    uint64_t password_hash;
    bool password_set;
};

void users_init(void);
const struct user_record *user_current(void);
int user_login(const char *name);
int user_login_password(const char *name, const char *password);
const struct user_record *user_find_name(const char *name);
const struct user_record *user_find_uid(uint32_t uid);
int user_add(const char *name, uint32_t uid, uint32_t gid);
int user_del(const char *name);
int user_set_password(const char *name, const char *password);
bool user_check_password(const char *name, const char *password);
bool user_has_password(const char *name);
size_t user_count(void);
const struct user_record *user_get(size_t index);

#endif
