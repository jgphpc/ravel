#ifndef TASKGROUP_H
#define TASKGROUP_H

#include <QString>
#include <QList>
#include <QMap>

class TaskGroup
{
public:
    TaskGroup(int _id, QString _name);
    ~TaskGroup() { delete tasks; }

    int id;
    QString name;
    // May need to keep additional names if we have several that are the same

    // This order is important for communicator rank ID
    QList<unsigned int> * tasks;
    QMap<unsigned int, int> * taskorder;

    // In the future - some sort of hierarchy links between communicators?


};

#endif // TASKGROUP_H