# HttpServer 重写



```c++


HttpServer server(HttpHandler, eventLoop, EventLoopGroup*);


server.add_endpoint(Http1Endpoint{});
server.add_endpoint(Http2Endpoint{});
server.add_endpoint(Http21Endpoint{});
server.add_endpoint(Http3Endpoint{});

server.serve();

```


Endpoint Ops 操作
```c++

struct EndpointOps {
    
    void on_start(void *); // 每个 endpoint 绑定
    Task<> on_serve(void *); // accept loop 
    void on_stop(void *); // notify close
    /* 以上三个函数在主线程执行一次 */
    
    /* 下面的函数每个 woker 执行一次 */
    void *on_create_worker_ctx(void *ctx);
    void on_worker_stop(void *ctx, void *worker_ctx);
    Task<> waiting_worker_stop(void *, void *worker_ctx);
};

```