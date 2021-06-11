#ifndef APPLOCKER_H
#define APPLOCKER_H

#include <QCloseEvent>
#include <QDesktopWidget>
#include <QIntValidator>
#include <QMessageBox>
#include <QPushButton>
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class AppLocker; }
QT_END_NAMESPACE

class AppLocker : public QWidget
{
    Q_OBJECT

public:
    AppLocker(QWidget *parent = nullptr);
    bool isWalletLocked(){return walletLocked;}
    ~AppLocker();

private:
    Ui::AppLocker *ui;
    QString pinCode;
    bool walletLocked = false;
    void setLock();

Q_SIGNALS:
    void lockingApp(bool);
    void quitAppFromWalletLocker();

public Q_SLOTS:
    void showLocker();

protected:
    void closeEvent(QCloseEvent *event) override;
};
#endif // APPLOCKER_H
