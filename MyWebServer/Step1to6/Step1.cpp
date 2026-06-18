#include <iostream>
#include <cstring>
#include <unistd.h> //provide: read, write, close
#include <sys/socket.h> // Provide: socket, bind, listen, accept
#include <netinet/in.h> // Proveide: sockaddr_in
#include <arpa/inet.h> // Provide: htons

/*
| 函数       | 作用                 | 生活类比            |
| ---------- | -------------------- | ------------------- |
| `socket()` | 创建一个 socket      | 买一部电话          |
| `bind()`   | 把 socket 和端口绑定 | 给电话分配号码 8080 |
| `listen()` | 开始监听             | 打开"等待来电"模式  |
| `accept()` | 接受一个客户端连接   | 接起一通电话        |
| `read()`   | 从连接读数据         | 听对方说话          |
| `write()`  | 向连接写数据         | 你回话              |
| `close()`  | 关闭连接或 socket    | 挂电话              |
*/
using namespace std;

int main()
{
    // 1. 创建socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        cerr<<"socket 创建失败\n";
        return 1;
    }

    // 2. 绑定端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    if(bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        cerr<<"bind 失败（端口可能被占用）\n";
        close(server_fd);
        return 1;
    }

    // 3.开始监听
    if(listen(server_fd, 10) < 0)
    {
        cerr<<"Listen 失败\n";
        close(server_fd);
        return 1;
    }
    cout<<"服务器已启动：http://127.0.0.1:8080\n";
    cout << "在浏览器打开上述地址，或用 curl 测试\n";

    // 4.接受客户端连接--循环
    while(true)
    {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        // 4.1 阻塞等待，直到有浏览器连进来
        int client_fd = accept(server_fd, (sockaddr*) &client_addr, &client_len);
        if(client_fd < 0)
        {
            cerr << "accept 失败\n";
            continue;  // 这次失败不退出，继续等下一个
        }

        cout << "收到新连接\n";

        // 4.2 读取Http请求
        char buffer[4096] = {0};
        ssize_t n = read(client_fd, buffer, sizeof(buffer)-1);
        if(n > 0)
        {
            cout << "收到请求（完整报文，共 " << n << " 字节）：\n";
            cout << string(buffer, n)<<"\n";
        }

        // 4.3 构建Http响应
        const char* body = "Hello World!";
        string reponse =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 12\r\n"
            "Connection: close\r\n"
            "\r\n"
            + std::string(body);
        
        write(client_fd, reponse.c_str(), reponse.size());
        
        // 4.4 close
        close(client_fd);
        cout << "已响应并关闭连接\n";
    }   

    close(server_fd);
    return 0;
}