#ifndef _SNOWFLAKE_H_
#define _SNOWFLAKE_H_

#include <iostream>
#include <chrono>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>

class SnowflakeIDGenerator {
public:
    SnowflakeIDGenerator(int machineId) : machineId(machineId) {
        epoch = 1609459200000L; // 设置纪元时间（例如，2021年1月1日0点）
        machineIdBits = 2; // 机器ID占2位
        sequenceBits = 4; // 序列号占4位
        maxMachineId = -1 ^ (-1 << machineIdBits); // 最大机器ID值 (3)
        sequenceMask = -1 ^ (-1 << sequenceBits); // 序列号掩码 (15)

        lastTimestamp = -1;
        sequence = 0;
    }

    std::string generateID() {
        long long timestamp = (currentTimeMillis() - epoch) / 1000;  // 以秒为单位的时间戳

        if (timestamp != lastTimestamp) {
            sequence = 0; // 重置序列号
            lastTimestamp = timestamp;
        } else if (sequence < sequenceMask) {
            sequence++; // 同一秒内，序列号递增
        } else {
            // 如果同一秒内的ID生成数已经达到上限，则等待下一秒
            timestamp = waitForNextSecond(lastTimestamp);
        }

        long long id = ((timestamp & ((1 << 5) - 1)) << (machineIdBits + sequenceBits)) // 时间戳部分
                       | ((machineId & maxMachineId) << sequenceBits)               // 机器ID部分
                       | (sequence & sequenceMask);                                  // 序列号部分

        // 将生成的ID转换为一个固定长度的数字字符串，保证11位
        return toFixedLengthString(id);
    }

private:
    int machineId; // 机器ID
    long long epoch; // 起始时间（纪元时间）
    int machineIdBits; // 机器ID的位数
    int sequenceBits; // 序列号的位数
    int maxMachineId; // 最大机器ID
    int sequenceMask; // 序列号掩码
    long long lastTimestamp; // 上一次生成ID的时间戳
    int sequence; // 当前秒的序列号

    long long currentTimeMillis() {
        using namespace std::chrono;
        return duration_cast<std::chrono::milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    long long waitForNextSecond(long long lastTimestamp) {
        long long timestamp = currentTimeMillis();
        while (timestamp <= lastTimestamp) {
            timestamp = currentTimeMillis(); // 等待直到下一秒
        }
        return timestamp;
    }

    // 将生成的ID转换为11位数字字符串
    std::string toFixedLengthString(long long number) {
        std::ostringstream oss;
        oss << number;

        // 如果ID的位数不足11位，补充前导零
        std::string id = oss.str();
        while (id.length() < 11) {
            id = "0" + id;
        }

        // 如果ID的位数超过11位，截取最后的11位
        if (id.length() > 11) {
            id = id.substr(id.length() - 11);
        }

        return id;
    }
};

#endif
