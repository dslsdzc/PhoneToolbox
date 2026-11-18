#include "secure/identity.h"
#include "server.h"
#include "util.h"

#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--port N] [--state-dir DIR] [--audit PATH]"
            " [--allow-plaintext] [--token HEX] [--pair-code CODE]\n"
            "       %s --list-peers --state-dir DIR\n"
            "       %s --remove-peer HEX --state-dir DIR\n"
            "  --port            listen port (0 = kernel assigned; printed as PORT=<n>)\n"
            "  --state-dir       identity.key / peers.txt directory\n"
            "  --audit           append-only audit log path\n"
            "  --allow-plaintext development mode (token auth, no Noise)\n"
            "  --token           token for --allow-plaintext (random when omitted)\n"
            "  --pair-code       short code for first-time CPace pairing\n"
            "  --list-peers      print paired computers (PEER=<hex>\\t<name>\\t<caps>)\n"
            "  --remove-peer     remove one paired computer by static public key hex\n",
            program, program);
}

int main(int argc, char **argv)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "libsodium initialization failed\n");
        return 1;
    }

    int port = 0;
    char *token = NULL;
    const char *audit_path = NULL;
    const char *state_dir = NULL;
    const char *pair_code = NULL;
    int allow_plaintext = 0;
    int list_peers = 0;
    const char *remove_peer_hex = NULL;
    char default_state[4096];

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            free(token);
            token = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--audit") == 0 && i + 1 < argc) {
            audit_path = argv[++i];
        } else if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            state_dir = argv[++i];
        } else if (strcmp(argv[i], "--allow-plaintext") == 0) {
            allow_plaintext = 1;
        } else if (strcmp(argv[i], "--pair-code") == 0 && i + 1 < argc) {
            pair_code = argv[++i];
        } else if (strcmp(argv[i], "--list-peers") == 0) {
            list_peers = 1;
        } else if (strcmp(argv[i], "--remove-peer") == 0 && i + 1 < argc) {
            remove_peer_hex = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            free(token);
            return 0;
        } else {
            usage(argv[0]);
            free(token);
            return 1;
        }
    }

    if (!state_dir) {
        const char *home = getenv("HOME");
        if (home && home[0])
            snprintf(default_state, sizeof(default_state), "%s/.phone-agent", home);
        else
            snprintf(default_state, sizeof(default_state), "/data/local/tmp/phone-agent");
        state_dir = default_state;
    }

    if (list_peers) {
        peer_record *peers = NULL;
        int count = 0;
        if (peers_load(state_dir, &peers, &count) != 0) {
            fprintf(stderr, "cannot read peers.txt\n");
            free(token);
            return 1;
        }
        for (int i = 0; i < count; ++i) {
            char hex[65];
            sodium_bin2hex(hex, sizeof(hex), peers[i].public_key,
                           AGENT_IDENTITY_KEY_LEN);
            printf("PEER=%s\t%s\t%s\n", hex, peers[i].name,
                   peers[i].capabilities);
        }
        peers_free(peers);
        free(token);
        return 0;
    }

    if (remove_peer_hex) {
        uint8_t public_key[AGENT_IDENTITY_KEY_LEN];
        if (strlen(remove_peer_hex) != AGENT_IDENTITY_KEY_LEN * 2
            || sodium_hex2bin(public_key, sizeof(public_key), remove_peer_hex,
                              strlen(remove_peer_hex), NULL, NULL, NULL) != 0) {
            fprintf(stderr, "invalid peer public key hex\n");
            free(token);
            return 1;
        }
        if (peers_remove(state_dir, public_key) != 0) {
            fprintf(stderr, "cannot remove peer\n");
            free(token);
            return 1;
        }
        printf("REMOVED=1\n");
        free(token);
        return 0;
    }

    int token_generated = 0;
    if (allow_plaintext && !token) {
        if (random_hex(32, &token) != 0) {
            fprintf(stderr, "cannot generate token\n");
            return 1;
        }
        token_generated = 1;
        printf("TOKEN=%s\n", token);
        fflush(stdout);
    }

    server_config config;
    memset(&config, 0, sizeof(config));
    config.port = port;
    config.token = token;
    config.audit_path = audit_path;
    config.state_dir = state_dir;
    config.pair_code = pair_code;
    config.allow_plaintext = allow_plaintext;

    const int result = server_run(&config);

    if (token_generated)
        free(token);
    return result == 0 ? 0 : 1;
}
