#include <qt/applocker.h>
#include <qt/forms/ui_applocker.h>

AppLocker::AppLocker(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::AppLocker)
{
    ui->setupUi(this);
    this->setWindowTitle("Wallet locker");
    this->setWindowModality(Qt::ApplicationModal);
    QIntValidator *validatorInt = new QIntValidator(1, 999999999, this);

    //lock view (index 1)
    ui->stackedWidget->setCurrentIndex(1);
    ui->headLabel->setText("Set a PIN code to lock your wallet:\n");
    ui->messageLabel->setText("\n- PIN code should be at least a 6 digit number.\n"
                              "- PIN is only valid for this session");
    ui->buttonBox->button(QDialogButtonBox::Ok)->setText("Lock");
    ui->pinLineEdit->setValidator(validatorInt);
    ui->pinLineEdit->setEchoMode(QLineEdit::Password);
    ui->confirmLineEdit->setValidator(validatorInt);
    ui->confirmLineEdit->setEchoMode(QLineEdit::Password);

    //unlock view
    ui->lockLabel->setText("Your wallet is locked.\n");
    ui->unlocklabel->setText("PIN");
    ui->unlockLineEdit->setValidator(validatorInt);
    ui->unlockLineEdit->setEchoMode(QLineEdit::Password);

    connect(ui->unlockLineEdit, &QLineEdit::textChanged, [this]{if(ui->unlockLineEdit->text().size() > 5) ui->buttonBox->setEnabled(true);});
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &AppLocker::setLock);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &AppLocker::close);
}

AppLocker::~AppLocker()
{
    delete ui;
}

void AppLocker::setLock()
{
    switch (ui->stackedWidget->currentIndex()) {
    case 0:
        if(ui->unlockLineEdit->text() == pinCode){
            walletLocked = false;
            pinCode.clear();
            ui->stackedWidget->setCurrentIndex(1);
            ui->unlockLineEdit->clear();
            ui->buttonBox->button(QDialogButtonBox::Ok)->setText("Lock");
            ui->buttonBox->button(QDialogButtonBox::Cancel)->setVisible(true);
        }else{
            QMessageBox::warning(this, "Error", "PIN code is not correct", QMessageBox::Ok);
        }
        break;
    case 1:
        if(ui->pinLineEdit->text().isEmpty() || ui->confirmLineEdit->text().isEmpty()){
            QMessageBox::information(this, "Empty field", "Please enter and confirm your pin code", QMessageBox::Ok);
            return;
        }else if(ui->pinLineEdit->text().size() < 5){
            QMessageBox::information(this, "Error", "PIN code must be at least 6 digits long", QMessageBox::Ok);
            return;
        }else if(ui->pinLineEdit->text() != ui->confirmLineEdit->text()){
            QMessageBox::warning(this, "Error", "PIN code doesn't match, please check again", QMessageBox::Ok);
            return;
        }else{
            walletLocked = true;
            pinCode = ui->pinLineEdit->text();
            ui->pinLineEdit->clear();
            ui->confirmLineEdit->clear();
            ui->stackedWidget->setCurrentIndex(0); // move to unlock view
            ui->buttonBox->button(QDialogButtonBox::Ok)->setText("Unlock");
            ui->buttonBox->button(QDialogButtonBox::Cancel)->setVisible(false);
            ui->buttonBox->setEnabled(false);
            ui->unlockLineEdit->setFocus();
        }
        break;
    }
}

void AppLocker::showLocker()
{
    this->move(QApplication::desktop()->screen()->rect().center() - this->rect().center());
    this->show();
}

void AppLocker::closeEvent(QCloseEvent *event)
{
    if(walletLocked){
        int ret =  QMessageBox::warning(this, "WARNING", "Wallet application will exit, continue?", QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
        if(ret == QMessageBox::Cancel){
            event->ignore();
        }else{
            Q_EMIT quitAppFromWalletLocker();
            event->accept();
        }
    }else if(ui->stackedWidget->currentIndex() == 1){
        event->accept();
    }else{
        event->ignore();
    }
}
