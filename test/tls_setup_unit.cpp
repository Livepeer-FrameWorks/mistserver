#include <mist/socket.h>

#include <cassert>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#if MBEDTLS_VERSION_MAJOR > 2
namespace {
  void exportKeys(void *, mbedtls_ssl_key_export_type, const unsigned char *, size_t, const unsigned char[32],
                  const unsigned char[32], mbedtls_tls_prf_types) {}
} // namespace
#elif HAVE_UPSTREAM_MBEDTLS_SRTP
namespace {
  int exportKeys(void *, const unsigned char *, const unsigned char *, size_t, size_t, size_t, const unsigned char[32],
                 const unsigned char[32], mbedtls_tls_prf_types) {
    return 0;
  }
} // namespace
#endif

int main() {
#if MBEDTLS_VERSION_MAJOR > 2 || HAVE_UPSTREAM_MBEDTLS_SRTP
  mbedtls_ssl_config activeConfig;
  mbedtls_ssl_config unusedConfig;
  mbedtls_ssl_context context;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context random;
  mbedtls_ssl_config_init(&activeConfig);
  mbedtls_ssl_config_init(&unusedConfig);
  mbedtls_ssl_init(&context);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&random);
  const unsigned char personalization[] = "tls setup unit test";
  assert(mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy, personalization, sizeof(personalization) - 1) == 0);
  assert(mbedtls_ssl_config_defaults(&activeConfig, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) == 0);
  mbedtls_ssl_conf_rng(&activeConfig, mbedtls_ctr_drbg_random, &random);
  int marker = 42;
  assert(Socket::setupTLSContext(&context, &activeConfig, exportKeys, &marker) == 0);
#if MBEDTLS_VERSION_MAJOR > 2
  assert(context.MBEDTLS_PRIVATE(f_export_keys) == exportKeys);
  assert(context.MBEDTLS_PRIVATE(p_export_keys) == &marker);
#else
  assert(activeConfig.f_export_keys_ext == exportKeys);
  assert(activeConfig.p_export_keys == &marker);
  assert(!unusedConfig.f_export_keys_ext);
  assert(!unusedConfig.p_export_keys);
#endif
  mbedtls_ssl_free(&context);
  mbedtls_ctr_drbg_free(&random);
  mbedtls_entropy_free(&entropy);
  mbedtls_ssl_config_free(&unusedConfig);
  mbedtls_ssl_config_free(&activeConfig);
#endif
  return 0;
}
