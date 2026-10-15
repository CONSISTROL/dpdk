#ifndef _HNS5_LOG_H_
#define _HNS5_LOG_H_

#include <rte_log.h>

#define hns5_dbg(dev, format, ...) \
    RTE_LOG(DEBUG, PMD, "%-5s|%-32s|%4u|%s| "format, "DEBUG", __FUNCTION__, __LINE__, dev->data->name, ##__VA_ARGS__)

#define hns5_info(dev, format, ...) \
    RTE_LOG(INFO, PMD, "%-5s|%-32s|%4u|%s| "format, "INFO", __FUNCTION__, __LINE__, dev->data->name, ##__VA_ARGS__)

#define hns5_warn(dev, format, ...) \
    RTE_LOG(WARNING, PMD, "%-5s|%-32s|%4u|%s| "format, "WARN", __FUNCTION__, __LINE__, dev->data->name, ##__VA_ARGS__)

#define hns5_err(dev, format, ...) \
    RTE_LOG(ERR, PMD, "%-5s|%-32s|%4u|%s| "format, "ERROR", __FUNCTION__, __LINE__, dev->data->name, ##__VA_ARGS__)

/**
 * @brief Log in RXTX path; avoid adding if possible
*/
#define hns5_rxtx(level, format, ...) \
    RTE_LOG(level, PMD, "%-5s|%-32s|%4u| "format, #level, __FUNCTION__, __LINE__, ##__VA_ARGS__)

/**
 * @brief Log macro without dev, for scenarios with no eth_dev (e.g. TM)
*/
#define hns5_log_info(format, ...) \
    RTE_LOG(INFO, PMD, "%-5s|%-32s|%4u| "format, "INFO", __FUNCTION__, __LINE__, ##__VA_ARGS__)

#define hns5_log_error(format, ...) \
    RTE_LOG(ERR, PMD, "%-5s|%-32s|%4u| "format, "ERROR", __FUNCTION__, __LINE__, ##__VA_ARGS__)

#endif /* _HNS5_LOG_H_ */
