# HttpServer 重写
server 主要负责 endpoint 管理，event loop group 通知停止。等待 各个worker 资源回收等

每个 endpoint 关联一个 listener，一个 serverOptions （包含tls，超时等配置）。


```c++


Server server(HttpHandler, eventLoop, EventLoopGroup*);


server.add_endpoint(Http1Endpoint{});
server.add_endpoint(Http2Endpoint{});
server.add_endpoint(Http21Endpoint{});
server.add_endpoint(Http3Endpoint{});

server.start();
co_await server.serve();



```


Endpoint Ops 操作，考虑下是像这样做成 Ops 还是直接做成 Endpoint 虚接口。
```c++

struct EndpointOps {
    
    void on_start(void *); // 每个 endpoint 绑定
    Task<> on_serve(void *); // accept loop 
    
    
    /**
     * notify close，主线程首先通知 close listener, 再通过各个worker 线程执行waiting_worker_stop 等待所有连接的请求执行完
    */
    void on_stop(void *); 
    /* 以上三个函数在主线程执行一次 */
    
    /* 下面的函数每个 woker 执行一次 */
    void *on_create_worker_ctx(void *ctx); // 创建单worker级别的ctx
    Task<> waiting_worker_stop(void *ctx, void *worker_ctx);// worker 要停止的时候通知，并且等待资源完全销毁
};

```

内部的 Server::Worker 设计
- event loop group 有几个 event loop 就有几个 worker，否则只有一个worker 和 http server 的 event loop 绑定
```c++
    struct HttpServer::OpsPair{
        EndpointOps ops;
        void *ctx;
    };
    class HttpServer::Worker {
        std::vector<OpsPair> worker_ops_list; // 有几个 endpoint 就有几个 模板
         
        void notify_stop();
        Task<> wait_stop();
    };
    

```