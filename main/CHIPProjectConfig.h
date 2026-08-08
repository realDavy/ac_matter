/*
 * Project-specific CHIP Device Layer overrides.
 */

#pragma once

/**
 * Vendor name exposed via Matter Basic Information (VendorName),
 * shown as Manufacturer in Apple Home / Google Home, etc.
 */
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "aidaegis"

/**
 * Product / device model name (Basic Information ProductName).
 * Also used as the default commissionable / NodeLabel-facing name.
 */
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "AC Remote"
#define CHIP_DEVICE_CONFIG_DEVICE_NAME "AC Remote"
