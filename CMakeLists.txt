cmake_minimum_required(VERSION 3.0)
project(WEBSERVER)
add_compile_options(-Wall -std=c++11)
add_executable(server1 main.cpp 
./CGImysql/sql_connection_pool.cpp  
./config.cpp 
./webserver.cpp 
./timer/lst_timer.cpp 
./http/http_conn.cpp 
./log/log.cpp)

include_directories(/usr/include/mysql)
link_directories(/usr/lib/x86_64-linux-gnu)
target_link_libraries(server1 PRIVATE mysqlclient)
target_link_libraries(server1 PRIVATE pthread)
include_directories(./lock ./CGImysql ./threadpool ./http ./timer ./log ./snowflake ./session)