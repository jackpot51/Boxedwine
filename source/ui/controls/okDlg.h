#ifndef __OK_DLG_H__
#define __OK_DLG_H__

class OkDlg : public BaseDlg {
public:
    OkDlg(int title, BString label, std::function<void()> onDone, int width=400, int height=150);

protected:
    virtual void run();

private:
    BString label;
    std::function<void()> onDone;
};

#endif