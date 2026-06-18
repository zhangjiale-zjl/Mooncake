#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>

namespace mooncake {

using ubsio_client_initFunc = int32_t (*)(int32_t, uint64_t);
using ubsio_putFunc = int32_t (*)(const char *, void *, size_t, uint32_t);
using ubsio_getFunc = int32_t (*)(const char *, void *, size_t, uint32_t);
using ubsio_existFunc = bool (*)(const char *, uint32_t);
using ubsio_deleteFunc = int32_t (*)(const char *, uint32_t);
using ubsio_get_lengthFunc = int32_t (*)(const char *, size_t *, uint32_t);
using ubsio_batch_putFunc = int32_t (*)(const char **, uint32_t, void **, size_t *, int *, uint32_t);
using ubsio_batch_getFunc = int32_t (*)(const char **, uint32_t, void **, size_t *, int *, uint32_t);
using ubsio_batch_get_hbmFunc = int32_t (*)(const char **, uint32_t, void ***, size_t **,
                                            uint32_t, uint32_t, int *, uint32_t);
using ubsio_batch_existFunc = int32_t (*)(const char **, uint32_t, bool *, uint32_t);
using ubsio_batch_deleteFunc = int32_t (*)(const char **, uint32_t, int32_t *, uint32_t);
using ubsio_batch_get_lengthFunc = int32_t (*)(const char **, uint32_t, size_t *, int32_t *, uint32_t);
using ubsio_batch_free_addressFunc = int32_t (*)(void **, uint32_t);

class DlUbsioApi {
public:
    static int32_t LoadLibrary();
    static void CleanupLibrary();

    static inline int32_t UbsioClientInit(int32_t deviceId, uint64_t ssdSize)
    {
        if (pUbsioClientInit == nullptr) {
            return -1;
        }
        return pUbsioClientInit(deviceId, ssdSize);
    }

    static inline int32_t UbsioPut(const char *key, void *buf, size_t length, uint32_t flags)
    {
        if (pUbsioPut == nullptr) {
            return -1;
        }
        return pUbsioPut(key, buf, length, flags);
    }

    static inline int32_t UbsioGet(const char *key, void *buf, size_t length, uint32_t flags)
    {
        if (pUbsioGet == nullptr) {
            return -1;
        }
        return pUbsioGet(key, buf, length, flags);
    }

    static inline bool UbsioExist(const char *key, uint32_t flags)
    {
        if (pUbsioExist == nullptr) {
            return false;
        }
        return pUbsioExist(key, flags);
    }

    static inline int32_t UbsioDelete(const char *key, uint32_t flags)
    {
        if (pUbsioDelete == nullptr) {
            return -1;
        }
        return pUbsioDelete(key, flags);
    }

    static inline int32_t UbsioGetLength(const char *key, size_t *length, uint32_t flags)
    {
        if (pUbsioGetLength == nullptr) {
            return -1;
        }
        return pUbsioGetLength(key, length, flags);
    }

    static inline int32_t UbsioBatchPut(const char **keys, uint32_t keys_count, void **bufs, size_t *lengths,
                                     int *results, uint32_t flags)
    {
        if (pUbsioBatchPut == nullptr) {
            return -1;
        }
        return pUbsioBatchPut(keys, keys_count, bufs, lengths, results, flags);
    }

    static inline int32_t UbsioBatchGet(const char **keys, uint32_t keys_count, void **bufs, size_t *lengths,
                                     int *results, uint32_t flags)
    {
        if (pUbsioBatchGet == nullptr) {
            return -1;
        }
        return pUbsioBatchGet(keys, keys_count, bufs, lengths, results, flags);
    }

    static inline int32_t UbsioBatchGetWithHBM(const char **keys, uint32_t keys_count, void ***bufs, size_t **lengths,
                                     uint32_t lengthsRows, uint32_t lengthsCols, int *results, uint32_t flags)
    {
        if (pUbsioBatchGetWithHBM == nullptr) {
            return -1;
        }
        return pUbsioBatchGetWithHBM(keys, keys_count, bufs, lengths, lengthsRows, lengthsCols, results, flags);
    }

    static inline int32_t UbsioBatchExist(const char **keys, uint32_t keys_count, bool *results, uint32_t flags)
    {
        if (pUbsioBatchExist == nullptr) {
            return -1;
        }
        return pUbsioBatchExist(keys, keys_count, results, flags);
    }

    static inline int32_t UbsioBatchDelete(const char **keys, uint32_t keys_count, int32_t *results, uint32_t flags)
    {
        if (pUbsioBatchDelete == nullptr) {
            return -1;
        }
        return pUbsioBatchDelete(keys, keys_count, results, flags);
    }

    static inline int32_t UbsioBatchGetLength(const char **keys, uint32_t keys_count, size_t *lengths,
                                           int32_t *results, uint32_t flags)
    {
        if (pUbsioBatchGetLength == nullptr) {
            return -1;
        }
        return pUbsioBatchGetLength(keys, keys_count, lengths, results, flags);
    }

    static inline int32_t UbsioBatchFreeAddress(void **bufs, uint32_t keys_count)
    {
        if (pUbsioBatchFreeAddress == nullptr) {
            return -1;
        }
        return pUbsioBatchFreeAddress(bufs, keys_count);
    }

private:
    static std::mutex gMutex;
    static bool gLoaded;
    static void *ubsioHandle;
    static const std::string gUbsioLibName;

    static ubsio_client_initFunc pUbsioClientInit;
    static ubsio_putFunc pUbsioPut;
    static ubsio_getFunc pUbsioGet;
    static ubsio_existFunc pUbsioExist;
    static ubsio_deleteFunc pUbsioDelete;
    static ubsio_get_lengthFunc pUbsioGetLength;
    static ubsio_batch_putFunc pUbsioBatchPut;
    static ubsio_batch_getFunc pUbsioBatchGet;
    static ubsio_batch_get_hbmFunc pUbsioBatchGetWithHBM;
    static ubsio_batch_existFunc pUbsioBatchExist;
    static ubsio_batch_deleteFunc pUbsioBatchDelete;
    static ubsio_batch_get_lengthFunc pUbsioBatchGetLength;
    static ubsio_batch_free_addressFunc pUbsioBatchFreeAddress;
};
}
