#include "app/App.h"

#include <csignal>

int main(int argc, char **argv) {
    (void) ::signal(SIGPIPE, SIG_IGN);
    fiber::lite_nginx::app::LiteNginxApp app;
    return app.run(argc, argv);
}
