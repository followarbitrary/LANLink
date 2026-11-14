#include <QApplication>
#include <QIcon>

#include "main_window.hpp"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("LANLink"));
  QApplication::setApplicationVersion(QStringLiteral("0.1"));
  app.setWindowIcon(QIcon(QStringLiteral(":/icons/lanlink-icon.png")));
  lanlink::desktop::MainWindow window;
  window.show();
  return app.exec();
}
