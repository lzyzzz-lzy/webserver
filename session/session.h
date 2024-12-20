#ifndef _SESSION_H_
#define _SESSION_H_


#include <iostream>
#include <string>
#include <unordered_map>
#include <ctime>
#include <sstream>
#include <random>
#include <iomanip>

#define SESSION_TIMEOUT 3600  // 1小时过期

class Session {
public:
    // Session 数据结构
    struct SessionData {
        std::string username;
        std::time_t last_access_time;
        // 可以在这里添加更多的用户数据
    };

    // 构造函数
    Session() = default;

    // 生成唯一的 Session ID
    std::string generate_session_id() {
        std::stringstream session_id;
        session_id << std::hex << std::time(nullptr) << rand();
        return session_id.str();
    }

    // 设置会话 ID 和存储 Session 数据
    void create_session(const std::string& session_id, const std::string& username) {
        SessionData new_session;
        new_session.username = username;
        new_session.last_access_time = std::time(nullptr);
        
        // 存储会话数据
        sessions_[session_id] = new_session;
    }

    // 检查 Session 是否有效
    bool is_session_valid(const std::string& session_id) {
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            std::time_t current_time = std::time(nullptr);
            // 如果 Session 超过了超时时间，则认为过期
            if (current_time - it->second.last_access_time < SESSION_TIMEOUT) {
                return true;
            } else {
                sessions_.erase(it);  // 删除过期的 Session
            }
        }
        return false;  // Session 不存在或已过期
    }

    // 获取当前 Session 的用户名
    std::string get_username(const std::string& session_id) {
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            return it->second.username;
        }
        return "";  // 无效 Session
    }

    // 更新 Session 访问时间
    void update_session(const std::string& session_id) {
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.last_access_time = std::time(nullptr);  // 更新访问时间
        }
    }

    // 设置 Cookie 响应头
    std::string set_session_cookie(const std::string& session_id) {
        std::stringstream cookie_header;
        cookie_header << "Set-Cookie: session_id=" << session_id << "; path=/; HttpOnly";
        return cookie_header.str();
    }

    // 删除 Session 数据
    void delete_session(const std::string& session_id) {
        sessions_.erase(session_id);
    }

private:
    // 存储所有 Session 数据的 Map，Session ID 为键，SessionData 为值
    std::unordered_map<std::string, SessionData> sessions_;
};


#endif