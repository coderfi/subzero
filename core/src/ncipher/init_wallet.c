#include <assert.h>
#include <examples/network_server/common.h>
#include <nfastapp.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <seelib.h>
#include <squareup/subzero/common.pb.h>
#include <squareup/subzero/internal.pb.h>
#include <stdio.h>

#include "bip32.h"
#include "bip39.h"
#include "checks.h"
#include "config.h"
#include "curves.h"
#include "init_wallet.h"
#include "log.h"
#include "protection.h"
#include "rand.h"
#include "rpc.h"

extern NFastApp_Connection conn;
extern NFast_AppHandle app;
extern M_CertificateList cert_list;

static Result gen_random(uint8_t *buffer, uint32_t buffer_len);

/**
 * Initialize a wallet.
 * in prod:
 *   1. generate 64 bytes of randomly generates bytes using the nCipher
 *   2. xor with the randomly generated bytes in the rpc.
 *   3. derive the pubkey
 *   4. encrypt master_seed
 *   5. encrypt pubkey
 *
 * in dev:
 *   - randomly generate 32 bytes
 *   - xor with the randomly generated bytes in the rpc.
 *   - derive the mnemonic. Print it out for debugging.
 *   - derive the master_seed
 *   - derive the pubkey
 *   - encrypt the master_seed and pubkey
 */
Result handle_init_wallet(InternalCommandRequest *in,
                          InternalCommandResponse_InitWalletResponse *out) {
  DEBUG("in handle_init_wallet");

  // 1. Read random bytes from nCipher
  uint8_t master_seed[MASTER_SEED_SIZE] = {0};
  Result r = gen_random(master_seed, sizeof(master_seed));
  if (r != Result_SUCCESS) {
    ERROR("generate_random_bytes failed (%d).", r);
    return r;
  }

  // 2. Mix random bytes from the host machine
  r = mix_entropy(master_seed, in);
  if (r != Result_SUCCESS) {
    ERROR("mix_entropy failed");
    return r;
  }

  // 3. Compute pubkey
  HDNode node;
  // TODO: error handling!
  hdnode_from_seed(master_seed, sizeof(master_seed), SECP256K1_NAME, &node);

  // We have to perform the first derivation (0' for Mainnet, 1' for Testnet) before getting
  // the pubkey
  // TODO: error handling!
  uint32_t fingerprint = hdnode_fingerprint(&node);
  hdnode_private_ckd_prime(&node, COIN_TYPE);
  hdnode_fill_public_key(&node);

  char pub_key[XPUB_SIZE];
  int ret = hdnode_serialize_public(&node, fingerprint, PUBKEY_PREFIX,
                                    pub_key, sizeof(pub_key));
  if (ret <= 0) {
    ERROR("hdnode_serialize_public failed");
    return Result_UNKNOWN_INTERNAL_FAILURE;
  }
  DEBUG("pub key m/1': %s", pub_key);

  // 4. encrypt master_seed
  r = protect_wallet(master_seed, &out->encrypted_master_seed);
  if (r != Result_SUCCESS) {
    ERROR("protect_wallet failed: (%d).", r);
    return r;
  }

  // 5. encrypt pubkey
  r = protect_pubkey(pub_key, &out->encrypted_pub_key);
  if (r != Result_SUCCESS) {
    ERROR("protect_pubkey failed: (%d).", r);
    return r;
  }

  return Result_SUCCESS;
}

uint8_t gen_random_buffer[256];
static Result gen_random(uint8_t *buffer, uint32_t buffer_len) {
  if (buffer_len > sizeof(gen_random_buffer)) {
    ERROR("buffer_len too large");
    return Result_GEN_RANDOM_BUFFER_TOO_LARGE_FAILURE;
  }
  M_Status retcode;
  M_Command command = {0};
  M_Reply reply = {0};

  command.cmd = Cmd_GenerateRandom;
  command.args.generaterandom.lenbytes = buffer_len;
  command.certs = &cert_list;
  command.flags |= Command_flags_certs_present;

  if ((retcode = NFastApp_Transact(conn, NULL, &command, &reply, NULL)) !=
      Status_OK) {
    ERROR("NFastApp_Transact failed");
    return Result_NFAST_APP_TRANSACT_FAILURE;
  }
  if ((retcode = reply.status) != Status_OK) {
    ERROR("NFastApp_Transact not ok");
    char buf[1000];
    NFast_StrError(buf, sizeof(buf), reply.status, NULL);
    ERROR("message: %s", buf);

    NFastApp_Free_Reply(app, NULL, NULL, &reply);
    return Result_NFAST_APP_TRANSACT_STATUS_FAILURE;
  }

  if (reply.reply.generaterandom.data.len != buffer_len) {
    ERROR("invalid data len");
    NFastApp_Free_Reply(app, NULL, NULL, &reply);
    return Result_GEN_RANDOM_UNEXPECTED_LEN_FAILURE;
  }
  memcpy(buffer, reply.reply.generaterandom.data.ptr, buffer_len);
  NFastApp_Free_Reply(app, NULL, NULL, &reply);

  return Result_SUCCESS;
}
