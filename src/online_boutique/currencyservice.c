/*
# Copyright 2022 University of California, Riverside
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include <rte_branch_prediction.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_memzone.h>

#include "c_lib.h"
#include "http.h"
#include "io.h"
#include "spright.h"
#include "utility.h"

static int pipefd_rx[UINT8_MAX][2];
static int pipefd_tx[UINT8_MAX][2];

char *currencies[] = {"EUR", "USD", "JPY", "CAD"};
double conversion_rate[] = {1.0, 1.1305, 126.40, 1.5128};

static int compare_e(void *left, void *right)
{
    return strcmp((const char *)left, (const char *)right);
}

struct clib_map *currency_data_map;

static void getCurrencyData(struct clib_map *map)
{
    int size = sizeof(currencies) / sizeof(currencies[0]);
    int i = 0;
    for (i = 0; i < size; i++)
    {
        char *key = clib_strdup(currencies[i]);
        int key_length = (int)strlen(key) + 1;
        double value = conversion_rate[i];
        log_info("Inserting [%s -> %f]", key, value);
        insert_c_map(map, key, key_length, &value, sizeof(double));
        free(key);
    }
}

static void GetSupportedCurrencies(struct http_transaction *in)
{
    log_info("[GetSupportedCurrencies] received request");

    in->get_supported_currencies_response.num_currencies = 0;
    int size = sizeof(currencies) / sizeof(currencies[0]);
    int i = 0;
    for (i = 0; i < size; i++)
    {
        in->get_supported_currencies_response.num_currencies++;
        strcpy(in->get_supported_currencies_response.CurrencyCodes[i], currencies[i]);
    }

    return;
}

/*
 * Helper function that handles decimal/fractional carrying
 */
static void Carry(Money *amount)
{
    double fractionSize = pow(10, 9);
    amount->Nanos = amount->Nanos + (int32_t)((double)(amount->Units % 1) * fractionSize);
    amount->Units = (int64_t)(floor((double)amount->Units) + floor((double)amount->Nanos / fractionSize));
    amount->Nanos = amount->Nanos % (int32_t)fractionSize;
    return;
}

static void Convert(struct http_transaction *txn)
{
    log_info("[Convert] received request");
    CurrencyConversionRequest *in = &txn->currency_conversion_req;
    Money *euros = &txn->currency_conversion_result;

    // printMoney(euros);
    // printCurrencyConversionRequest(in);

    // Convert: from_currency --> EUR
    void *data;
    find_c_map(currency_data_map, in->From.CurrencyCode, &data);
    euros->Units = (int64_t)((double)in->From.Units / *(double *)data);
    euros->Nanos = (int32_t)((double)in->From.Nanos / *(double *)data);

    Carry(euros);
    euros->Nanos = (int32_t)(round((double)euros->Nanos));

    // Convert: EUR --> to_currency
    find_c_map(currency_data_map, in->ToCode, &data);
    euros->Units = (int64_t)((double)euros->Units / *(double *)data);
    euros->Nanos = (int32_t)((double)euros->Nanos / *(double *)data);
    Carry(euros);

    euros->Units = (int64_t)(floor((double)(euros->Units)));
    euros->Nanos = (int32_t)(floor((double)(euros->Nanos)));
    strcpy(euros->CurrencyCode, in->ToCode);

    log_info("[Convert] completed request");
    return;
}

static void *nf_worker(void *arg)
{
    struct http_transaction *txn = NULL;
    ssize_t bytes_written;
    ssize_t bytes_read;
    uint8_t index;

    /* TODO: Careful with this pointer as it may point to a stack */
    index = (uint64_t)arg;

    while (1)
    {
        bytes_read = read(pipefd_rx[index][0], &txn, sizeof(struct http_transaction *));
        if (unlikely(bytes_read == -1))
        {
            log_error("read() error: %s", strerror(errno));
            return NULL;
        }

        if (strcmp(txn->rpc_handler, "GetSupportedCurrencies") == 0)
        {
            GetSupportedCurrencies(txn);
        }
        else if (strcmp(txn->rpc_handler, "Convert") == 0)
        {
            Convert(txn);
        }
        else
        {
            log_info("%s() is not supported", txn->rpc_handler);
            log_info("\t\t#### Run Mock Test ####");
            GetSupportedCurrencies(txn);
            PrintSupportedCurrencies(txn);
            MockCurrencyConversionRequest(txn);
            Convert(txn);
            PrintConversionResult(txn);
        }

        txn->next_fn = txn->caller_fn;
        txn->caller_fn = CURRENCY_SVC;

        bytes_written = write(pipefd_tx[index][1], &txn, sizeof(struct http_transaction *));
        if (unlikely(bytes_written == -1))
        {
            log_error("write() error: %s", strerror(errno));
            return NULL;
        }
    }

    return NULL;
}

static void *nf_rx(void *arg)
{
    struct http_transaction *txn = NULL;
    ssize_t bytes_written;
    uint8_t i;
    int ret;

    for (i = 0;; i = (i + 1) % cfg->nf[fn_id - 1].n_threads)
    {
        ret = io_rx((void **)&txn);
        if (unlikely(ret == -1))
        {
            log_error("io_rx() error");
            return NULL;
        }

        bytes_written = write(pipefd_rx[i][1], &txn, sizeof(struct http_transaction *));
        if (unlikely(bytes_written == -1))
        {
            log_error("write() error: %s", strerror(errno));
            return NULL;
        }
    }

    return NULL;
}

static void *nf_tx(void *arg)
{
    struct epoll_event event[UINT8_MAX]; /* TODO: Use Macro */
    struct http_transaction *txn = NULL;
    ssize_t bytes_read;
    uint8_t i;
    int n_fds;
    int epfd;
    int ret;

    epfd = epoll_create1(0);
    if (unlikely(epfd == -1))
    {
        log_error("epoll_create1() error: %s", strerror(errno));
        return NULL;
    }

    for (i = 0; i < cfg->nf[fn_id - 1].n_threads; i++)
    {
        ret = set_nonblocking(pipefd_tx[i][0]);
        if (unlikely(ret == -1))
        {
            return NULL;
        }

        event[0].events = EPOLLIN;
        event[0].data.fd = pipefd_tx[i][0];

        ret = epoll_ctl(epfd, EPOLL_CTL_ADD, pipefd_tx[i][0], &event[0]);
        if (unlikely(ret == -1))
        {
            log_error("epoll_ctl() error: %s", strerror(errno));
            return NULL;
        }
    }

    while (1)
    {
        n_fds = epoll_wait(epfd, event, cfg->nf[fn_id - 1].n_threads, -1);
        if (unlikely(n_fds == -1))
        {
            log_error("epoll_wait() error: %s", strerror(errno));
            return NULL;
        }

        for (i = 0; i < n_fds; i++)
        {
            bytes_read = read(event[i].data.fd, &txn, sizeof(struct http_transaction *));
            if (unlikely(bytes_read == -1))
            {
                log_error("read() error: %s", strerror(errno));
                return NULL;
            }

            log_debug("Route id: %u, Hop Count %u, Next Hop: %u, Next Fn: %u, \
                Caller Fn: %s (#%u), RPC Handler: %s()",
                      txn->route_id, txn->hop_count, cfg->route[txn->route_id].hop[txn->hop_count], txn->next_fn,
                      txn->caller_nf, txn->caller_fn, txn->rpc_handler);

            ret = io_tx(txn, txn->next_fn);
            if (unlikely(ret == -1))
            {
                log_error("io_tx() error");
                return NULL;
            }
        }
    }

    return NULL;
}

/* TODO: Cleanup on errors */
static int nf(uint8_t nf_id)
{
    const struct rte_memzone *memzone = NULL;
    pthread_t thread_worker[UINT8_MAX];
    pthread_t thread_rx;
    pthread_t thread_tx;
    uint8_t i;
    int ret;

    fn_id = nf_id;

    memzone = rte_memzone_lookup(MEMZONE_NAME);
    if (unlikely(memzone == NULL))
    {
        log_error("rte_memzone_lookup() error");
        return -1;
    }

    cfg = memzone->addr;

    ret = io_init();
    if (unlikely(ret == -1))
    {
        log_error("io_init() error");
        return -1;
    }

    for (i = 0; i < cfg->nf[fn_id - 1].n_threads; i++)
    {
        ret = pipe(pipefd_rx[i]);
        if (unlikely(ret == -1))
        {
            log_error("pipe() error: %s", strerror(errno));
            return -1;
        }

        ret = pipe(pipefd_tx[i]);
        if (unlikely(ret == -1))
        {
            log_error("pipe() error: %s", strerror(errno));
            return -1;
        }
    }

    ret = pthread_create(&thread_rx, NULL, &nf_rx, NULL);
    if (unlikely(ret != 0))
    {
        log_error("pthread_create() error: %s", strerror(ret));
        return -1;
    }

    ret = pthread_create(&thread_tx, NULL, &nf_tx, NULL);
    if (unlikely(ret != 0))
    {
        log_error("pthread_create() error: %s", strerror(ret));
        return -1;
    }

    for (i = 0; i < cfg->nf[fn_id - 1].n_threads; i++)
    {
        ret = pthread_create(&thread_worker[i], NULL, &nf_worker, (void *)(uint64_t)i);
        if (unlikely(ret != 0))
        {
            log_error("pthread_create() error: %s", strerror(ret));
            return -1;
        }
    }

    for (i = 0; i < cfg->nf[fn_id - 1].n_threads; i++)
    {
        ret = pthread_join(thread_worker[i], NULL);
        if (unlikely(ret != 0))
        {
            log_error("pthread_join() error: %s", strerror(ret));
            return -1;
        }
    }

    ret = pthread_join(thread_rx, NULL);
    if (unlikely(ret != 0))
    {
        log_error("pthread_join() error: %s", strerror(ret));
        return -1;
    }

    ret = pthread_join(thread_tx, NULL);
    if (unlikely(ret != 0))
    {
        log_error("pthread_join() error: %s", strerror(ret));
        return -1;
    }

    for (i = 0; i < cfg->nf[fn_id - 1].n_threads; i++)
    {
        ret = close(pipefd_rx[i][0]);
        if (unlikely(ret == -1))
        {
            log_error("close() error: %s", strerror(errno));
            return -1;
        }

        ret = close(pipefd_rx[i][1]);
        if (unlikely(ret == -1))
        {
            log_error("close() error: %s", strerror(errno));
            return -1;
        }

        ret = close(pipefd_tx[i][0]);
        if (unlikely(ret == -1))
        {
            log_error("close() error: %s", strerror(errno));
            return -1;
        }

        ret = close(pipefd_tx[i][1]);
        if (unlikely(ret == -1))
        {
            log_error("close() error: %s", strerror(errno));
            return -1;
        }
    }

    ret = io_exit();
    if (unlikely(ret == -1))
    {
        log_error("io_exit() error");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    log_set_level_from_env();

    uint8_t nf_id;
    int ret;

    ret = rte_eal_init(argc, argv);
    if (unlikely(ret == -1))
    {
        log_error("rte_eal_init() error: %s", rte_strerror(rte_errno));
        goto error_0;
    }

    argc -= ret;
    argv += ret;

    if (unlikely(argc == 1))
    {
        log_error("Network Function ID not provided");
        goto error_1;
    }

    errno = 0;
    nf_id = strtol(argv[1], NULL, 10);
    if (unlikely(errno != 0 || nf_id < 1))
    {
        log_error("Invalid value for Network Function ID");
        goto error_1;
    }

    currency_data_map = new_c_map(compare_e, NULL, NULL);
    getCurrencyData(currency_data_map);

    ret = nf(nf_id);
    if (unlikely(ret == -1))
    {
        log_error("nf() error");
        goto error_1;
    }

    ret = rte_eal_cleanup();
    if (unlikely(ret < 0))
    {
        log_error("rte_eal_cleanup() error: %s", rte_strerror(-ret));
        goto error_0;
    }

    return 0;

error_1:
    rte_eal_cleanup();
error_0:
    return 1;
}
