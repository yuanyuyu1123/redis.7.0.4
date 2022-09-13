#ifndef __CLICOMMON_H
#define __CLICOMMON_H

#include <hiredis.h>
#include <sdscompat.h> /* Use hiredis' sds compat header that maps sds calls to their hi_ variants */

typedef struct cliSSLconfig {
    /* Requested SNI, or NULL 请求的 SNI，或 NULL*/
    char *sni;
    /* CA Certificate file, or NULL
     * CA 证书文件，或 NULL*/
    char *cacert;
    /* Directory where trusted CA certificates are stored, or NULL
     * 存储受信任 CA 证书的目录，或 NULL*/
    char *cacertdir;
    /* Skip server certificate verification. 跳过服务器证书验证。*/
    int skip_cert_verify;
    /* Client certificate to authenticate with, or NULL
     * 要进行身份验证的客户端证书，或 NULL*/
    char *cert;
    /* Private key file to authenticate with, or NULL
     * 用于验证的私钥文件，或 NULL*/
    char *key;
    /* Preferred cipher list, or NULL (applies only to <= TLSv1.2)
     * 首选密码套件列表，或 NULL（仅适用于 TLSv1.3）*/
    char* ciphers;
    /* Preferred ciphersuites list, or NULL (applies only to TLSv1.3) */
    char* ciphersuites;
} cliSSLconfig;


/* server connection information object, used to describe an ip:port pair, db num user input, and user:pass. */
//服务器连接信息对象，用于描述 ip:port 对、db num 用户输入和 user:pass。
typedef struct cliConnInfo {
    char *hostip;
    int hostport;
    int input_dbnum;
    char *auth;
    char *user;
} cliConnInfo;

int cliSecureConnection(redisContext *c, cliSSLconfig config, const char **err);

ssize_t cliWriteConn(redisContext *c, const char *buf, size_t buf_len);

int cliSecureInit();

sds readArgFromStdin(void);

sds *getSdsArrayFromArgv(int argc,char **argv, int quoted);

sds unquoteCString(char *str);

void parseRedisUri(const char *uri, const char* tool_name, cliConnInfo *connInfo, int *tls_flag);

void freeCliConnInfo(cliConnInfo connInfo);

sds escapeJsonString(sds s, const char *p, size_t len);

#endif /* __CLICOMMON_H */
