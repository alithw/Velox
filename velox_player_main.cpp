#include <QApplication>
#include <QStringList>

#include "VeloxQtPlayerWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    VeloxQtPlayerWindow window;

    QStringList args = app.arguments();
    if (args.size() > 1)
    {
        QStringList paths = args.mid(1);
        window.addFiles(paths);
        window.playIndex(0);
    }

    window.resize(640, 520);
    window.show();
    return app.exec();
}
