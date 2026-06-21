#include "HttpHandler.h"
#include "HttpResponse.h"
#include "SqlPool.h"
#include "FormParser.h"
#include "Log.h"

using namespace std;

static string buildGetResponse(const HttpRequest& req)
{
    if(!isPathSafe(req.path))
    {
        return buildHttpResponse(
            403, "Forbidden",
            "text/plain; charset=utf-8",
            "403 - 非法路径"
        );
    }

    string filePath = pathToFile(req.path);
    string body;
    if(!readFile(filePath, body))
    {
        return buildHttpResponse(
            404, "Not Found", 
            "text/plain; charset=utf-8",
            "404 - 文件不存在：" + req.path
        );
    }

    return buildHttpResponse(200, "OK", getContentType(filePath), body);
}

static string buildPostEchoResponse(const HttpRequest& req)
{
    string msg = "收到 POST body:\n" + req.body;
    return buildHttpResponse(200, "OK", "text/plain; charset=utf-8", msg);
}

string handleRequest(const HttpRequest& req)
{
    return buildGetResponse(req);
}

string do_request(const HttpRequest& req)
{
    if(req.method == "GET")
        return buildGetResponse(req);
    if(req.method == "POST")
    {
        if(req.path == "/echo")
            return buildPostEchoResponse(req);
        if (req.path == "/register")
            return handle_register(req);
        if (req.path == "/login")
            return handle_login(req);
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8", "未知 POST 路径: " + req.path);
    }
    return buildHttpResponse(405, "Method Not Allowed",
        "text/plain; charset=utf-8", "暂只支持 GET 和 POST /echo");
}

string handle_register(const HttpRequest& req)
{
    auto form = parse_form_urlencoded(req.body);
    string user = form["username"];
    string pass = form["passwd"];
    if (user.empty() || pass.empty())
        return buildRedirectResponse("/register_error.html");

    ConnectionRAII raii;
    MYSQL* conn = raii.conn;
    string sql = "INSERT INTO user(username, password) VALUES('"
        + escape_sql(user) + "','" + escape_sql(pass) + "')";
    if (mysql_query(conn, sql.c_str()) != 0) {
    LOG_WARN(string("注册失败: ") + mysql_error(conn));
    return buildRedirectResponse("/register_error.html");
    }
    LOG_INFO("注册成功 user=" + user);
    return buildRedirectResponse("/welcome.html");
}

string handle_login(const HttpRequest& req) {
    auto form = parse_form_urlencoded(req.body);
    string user = form["username"];
    string pass = form["passwd"];
    if (user.empty() || pass.empty())
        return buildRedirectResponse("/login_error.html");

    ConnectionRAII raii;
    MYSQL* conn = raii.conn;
    string sql = "SELECT password FROM user WHERE username='"
               + escape_sql(user) + "' LIMIT 1";
    if (mysql_query(conn, sql.c_str()) != 0) {
        LOG_ERROR(string("登录查询失败: ") + mysql_error(conn));
        return buildRedirectResponse("/login_error.html");
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) return buildRedirectResponse("/login_error.html");

    MYSQL_ROW row = mysql_fetch_row(res);
    bool ok = row && row[0] && string(row[0]) == pass;
    mysql_free_result(res);

    if (ok) {
        LOG_INFO("登录成功 user=" + user);
        return buildRedirectResponse("/welcome.html");
    }
    LOG_WARN("登录失败 user=" + user);
    return buildRedirectResponse("/login_error.html");
}