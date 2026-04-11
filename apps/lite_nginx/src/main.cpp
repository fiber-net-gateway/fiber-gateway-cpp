#include "app/App.h"

int main(int argc, char **argv) {
    fiber::lite_nginx::app::LiteNginxApp app;
    return app.run(argc, argv);
}
