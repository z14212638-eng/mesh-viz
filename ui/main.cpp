// SPDX-License-Identifier: MIT
#include "meshviz-window.h"

#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <iostream>

int
main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc < 2)
    {
        std::cerr << "Usage: meshviz-viewer RUN.jsonl [--export DIRECTORY]\n";
        return 2;
    }
    try
    {
        MeshvizWindow window(QString::fromLocal8Bit(argv[1]));
        window.show();
        if (argc == 4 && QString(argv[2]) == "--export")
        {
            QString dir = QString::fromLocal8Bit(argv[3]);
            QTimer::singleShot(300, &app, [&app, &window, dir] {
                app.exit(window.exportViews(dir) ? 0 : 1);
            });
        }
        return app.exec();
    }
    catch (const std::exception& e)
    {
        std::cerr << "MeshViz: " << e.what() << '\n';
        return 1;
    }
}
