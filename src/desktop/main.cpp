#include <QApplication>

#include "main_window.hpp"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("LANLink"));
  QApplication::setApplicationVersion(QStringLiteral("0.1"));
  lanlink::desktop::MainWindow window;
  window.show();
  return app.exec();
}
