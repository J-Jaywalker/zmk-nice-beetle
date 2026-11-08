/* sim: minimal stand-in for <zephyr/net_buf.h> (Zephyr 4.x location); the
 * network buffer API is not used by the simulated code, the Bluetooth headers
 * only need the type names. */
#pragma once
#include <zephyr/kernel.h>
struct net_buf;
struct net_buf_pool;
struct net_buf_simple;
