#ifndef QT_SOUNDGAIN_HPP
#define QT_SOUNDGAIN_HPP

#include <QDialog>

namespace Ui {
class SoundGain;
}

class SoundGain : public QDialog {
    Q_OBJECT

public:
    explicit SoundGain(QWidget *parent = nullptr);
    ~SoundGain();

private slots:
    void on_verticalSlider_valueChanged(int value);

    void on_SoundGain_accepted();
    void on_SoundGain_rejected();

private:
    Ui::SoundGain *ui;
    int            sound_gain_orig;

    /* Drive and fan sound levels: the setting each slider edits, and its
       value when the dialog opened (restored on Cancel) */
    struct Level {
        int *value;
        int  orig;
    };
    Level levels[4];
};

#endif // QT_SOUNDGAIN_HPP
