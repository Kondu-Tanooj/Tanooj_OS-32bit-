#ifndef AUTH_H
#define AUTH_H

/* Returns 1 if authentication passed, 0 if failed */
int auth_boot(void);

/* Change the boot password (called from shell) */
void auth_set_password(const char *newpass);

#endif
