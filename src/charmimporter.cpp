#include "charmimporter.h"
#include "eventrecord.h"
#include "commrecord.h"
#include "rpartition.h"
#include "commevent.h"
#include "message.h"
#include "p2pevent.h"
#include "event.h"
#include "taskgroup.h"
#include <iostream>
#include <QStringList>
#include <QDir>
#include <QFileInfo>
#include <QStack>

#include "general_util.h"

CharmImporter::CharmImporter()
    : chares(new QMap<int, Chare*>()),
      entries(new QMap<int, Entry*>()),
      version(0),
      processes(0),
      trace(NULL),
      unmatched_msgs(NULL),
      charm_events(NULL),
      task_events(NULL),
      messages(new QVector<CharmMsg *>()),
      tasks(new QMap<int, Task *>()),
      taskgroups(new QMap<int, TaskGroup *>()),
      functiongroups(new QMap<int, QString>()),
      functions(new QMap<int, Function *>()),
      chare_to_task(QMap<QString, int>())
{
}

CharmImporter::~CharmImporter()
{
    for (QMap<int, Chare*>::Iterator itr = chares->begin();
         itr != chares->end(); ++itr)
    {
        delete itr.value();
    }
    delete chares;

    for (QMap<int, Entry*>::Iterator itr = entries->begin();
         itr != entries->end(); ++itr)
    {
        delete itr.value();
    }
    delete entries;

    for (QVector<CharmMsg *>::Iterator itr = messages->begin();
         itr != messages->end(); ++itr)
    {
        delete *itr;
    }
}

void CharmImporter::importCharmLog(QString dataFileName, OTFImportOptions * _options)
{
    readSts(dataFileName);

    unmatched_msgs = new QVector<QMap<int, QList<CharmMsg *> *> *>(processes);
    charm_events = new QVector<QVector<CharmEvt *> *>(processes);
    for (int i = 0; i < processes; i++) {
        (*unmatched_msgs)[i] = new QMap<int, QList<CharmMsg *> *>();
        (*charm_events)[i] = new QVector<CharmEvt *>();
    }

    processDefinitions();

    // Read individual log files
    QFileInfo file_info = QFileInfo(dataFileName);
    QDir directory = file_info.dir();
    QString basename = file_info.baseName();
    QString suffix = ".log";
    QString path = file_info.absolutePath();
    bool gzflag = false;
    if (directory.exists(basename + ".0.log.gz"))
    {
        gzflag = true;
        suffix += ".gz";
    }
    for (int i = 0; i < processes; i++)
        readLog(path + "/" + basename + "." + QString::number(i) + suffix,
                gzflag, i);

    // At this point, I have a list of events per PE
    // Now I have to check to see what chares are actually arrays
    // and then convert these by-PE events into by-chare
    // events based on these arrays. I should also make taskgroups and stuff
    int num_tasks = makeTasks();

    // Now that we have num_tasks, create the rawtrace to hold the conversion
    trace = new Trace(num_tasks);
    trace->units = 9;
    trace->functions = functions;
    trace->functionGroups = functiongroups;
    trace->taskgroups = taskgroups;
    trace->tasks = tasks;
    trace->collective_definitions = new QMap<int, OTFCollective *>();
    trace->collectives = new QMap<unsigned long long, CollectiveRecord *>();
    //trace->counters = new QMap<unsigned int, Counter *>();
    //trace->counter_records = new QVector<QVector<CounterRecord *> *>(num_tasks);
    trace->collectiveMap = new QVector<QMap<unsigned long long, CollectiveRecord *> *>(num_tasks);
    task_events = new QVector<QVector<CharmEvt *> *>(num_tasks);
    for (int i = 0; i < num_tasks; i++) {
        (*(trace->collectiveMap))[i] = new QMap<unsigned long long, CollectiveRecord *>();
        (*(trace->events))[i] = new QVector<Event *>();
        (*(trace->roots))[i] = new QVector<Event *>();
        (*(task_events))[i] = new QVector<CharmEvt *>();
    }

    // Change per-PE stuff to per-chare stuff
    charify();

    // Build partitions
    buildPartitions();

    // Delete newly created temp stuff
    cleanUp();
}

void CharmImporter::cleanUp()
{
    for (QVector<QMap<int, QList<CharmMsg *> *> *>::Iterator itr
         = unmatched_msgs->begin(); itr != unmatched_msgs->end(); ++itr)
    {
        for (QMap<int, QList<CharmMsg *> *>::Iterator eitr
             = (*itr)->begin(); eitr != (*itr)->end(); ++eitr)
        {
            // CharmMsgs deleted in charm events;
            delete *eitr;
        }
        delete *itr;
    }
    delete unmatched_msgs;

    for (QVector<QVector<CharmEvt *> *>::Iterator itr
         = charm_events->begin(); itr != charm_events->end(); ++itr)
    {
        for (QVector<CharmEvt *>::Iterator eitr
             = (*itr)->begin(); eitr != (*itr)->end(); ++eitr)
        {
            delete *eitr;
        }
        delete *itr;
    }
    delete charm_events;

    for (QVector<QVector<CharmEvt *> *>::Iterator itr
         = task_events->begin(); itr != task_events->end(); ++itr)
    {
        // Evts deleted in charm_events
        delete *itr;
    }
    delete task_events;
}

void CharmImporter::readLog(QString logFileName, bool gzipped, int pe)
{
    last = NULL;
    if (gzipped)
    {
        gzFile logfile = gzopen(logFileName.toStdString().c_str(), "r");
        char * buffer = new char[1024];
        gzgets(logfile, buffer, 1024); // Skip first line
        while (gzgets(logfile, buffer, 1024))
            parseLine(QString::fromUtf8(buffer).simplified(), pe);

        gzclose(logfile);
    }
    else
    {
        std::ifstream logfile(logFileName.toStdString().c_str());
        std::string line;
        std::getline(logfile, line); // Skip first line
        while(std::getline(logfile, line))
            parseLine(QString::fromStdString(line), pe);

        logfile.close();
    }

}

// Convert per-PE charm Events into per-chare events
void CharmImporter::charify()
{
    // Convert PE events into Chare events
    for (QVector<QVector<CharmEvt *> *>::Iterator event_list = charm_events->begin();
         event_list != charm_events->end(); ++event_list)
    {
        for (QVector<CharmEvt *>::Iterator event = (*event_list)->begin();
             event != (*event_list)->end(); ++event)
        {
            // Skip non-task events
            if (!(chare_to_task.contains((*event)->indexToString())))
            {
                continue;
            }

            int taskid = chare_to_task.value((*event)->indexToString());
            (*event)->task = taskid;
            task_events->at(taskid)->append(*event);

            std::cout << "Adding ";
            if ((*event)->enter)
            {
                std::cout << " enter ";
            }
            else
            {
                std::cout << " leave ";
            }
            std::cout << entries->value((*event)->entry)->name.toStdString().c_str();
            std::cout << " on index " << (*event)->indexToString().toStdString().c_str();
            std::cout << " with taskid " << taskid << std::endl;
        }
    }

    // Sort chare events in time
    for (QVector<QVector<CharmEvt *> *>::Iterator event_list
         = task_events->begin(); event_list != task_events->end();
         ++event_list)
    {
        std::cout << "Sorting !!!! " << std::endl;
        qSort((*event_list)->begin(), (*event_list)->end(), dereferencedLessThan<CharmEvt>);
        for (QVector<CharmEvt *>::Iterator ev = (*event_list)->begin();
             ev != (*event_list)->end(); ++ev)
        {
            if ((*ev)->enter)
            {
                std::cout << "Enter ";
            }
            else
            {
                std::cout << "Leave ";
            }
            std::cout << entries->value((*ev)->entry)->name.toStdString().c_str();
            std::cout << " on index " << (*ev)->indexToString().toStdString().c_str();
            std::cout << " with taskid " << (*ev)->task << std::endl;
        }
    }
}

void CharmImporter::makeSingletonPartition(CommEvent * evt)
{
    Partition * p = new Partition();
    p->addEvent(evt);
    evt->partition = p;
    p->new_partition = p;
    trace->partitions->append(p);
}

void CharmImporter::buildPartitions()
{
    QStack<CharmEvt *> stack = QStack<CharmEvt *>();
    int depth, phase;
    CharmEvt * bgn = NULL;
    CommEvent * prev = NULL;
    unsigned long long endtime = 0;
    int counter = 0;

    // Go through each separately.
    for (QVector<QVector<CharmEvt *> *>::Iterator event_list = task_events->begin();
         event_list != task_events->end(); ++event_list)
    {
        stack.clear();
        depth = 0;
        phase = 0;
        prev = NULL;
        std::cout << "Now handling " << counter << std::endl;
        counter++;
        for (QVector<CharmEvt *>::Iterator evt = (*event_list)->begin();
             evt != (*event_list)->end(); ++evt)
        {
            if ((*evt)->enter)
            {
                stack.push(*evt);
                std::cout << "Enter " << entries->value((*evt)->entry)->name.toStdString().c_str() << std::endl;
                std::cout << "     Stack size: " << stack.size() << std::endl;
                depth++;
            }
            else
            {
                std::cout << "Leave " << entries->value((*evt)->entry)->name.toStdString().c_str() << std::endl;
                std::cout << "     Stack size: " << stack.size() << std::endl;
                bgn = stack.pop();
                Event * e = NULL;
                if (trace->functions->value(bgn->entry)->group == 0) // Send or Recv
                {
                    QVector<Message *> * msgs = new QVector<Message *>();
                    if (bgn->charmmsg->tracemsg)
                    {
                        msgs->append(bgn->charmmsg->tracemsg);
                    }
                    else
                    {
                        Message * msg = new Message(bgn->charmmsg->sendtime,
                                                    bgn->charmmsg->recvtime,
                                                    0);
                        bgn->charmmsg->tracemsg = msg;
                        msgs->append(msg);
                    }
                    if (bgn->entry == SEND_FXN)
                    {
                        bgn->charmmsg->tracemsg->sender = new P2PEvent(bgn->time,
                                                                       (*evt)->time-1,
                                                                       bgn->entry,
                                                                       bgn->task,
                                                                       phase,
                                                                       msgs);

                        bgn->charmmsg->tracemsg->sender->comm_prev = prev;
                        if (prev)
                            prev->comm_next = bgn->charmmsg->tracemsg->sender;
                        prev = bgn->charmmsg->tracemsg->sender;

                        makeSingletonPartition(bgn->charmmsg->tracemsg->sender);

                        e = bgn->charmmsg->tracemsg->sender;

                    }
                    else if (bgn->entry == RECV_FXN)
                    {
                        bgn->charmmsg->tracemsg->receiver = new P2PEvent(bgn->time-1,
                                                                         (*evt)->time-2,
                                                                         bgn->entry,
                                                                         bgn->task,
                                                                         phase,
                                                                         msgs);

                        bgn->charmmsg->tracemsg->receiver->is_recv = true;

                        bgn->charmmsg->tracemsg->receiver->comm_prev = prev;
                        if (prev)
                            prev->comm_next = bgn->charmmsg->tracemsg->receiver;
                        prev = bgn->charmmsg->tracemsg->receiver;

                        makeSingletonPartition(bgn->charmmsg->tracemsg->receiver);

                        e = bgn->charmmsg->tracemsg->receiver;
                    }
                }
                else // Non-comm event
                {
                    e = new Event(bgn->time, (*evt)->time, bgn->entry, bgn->task);
                }

                depth--;
                e->depth = depth;
                if (depth == 0)
                    (*(trace->roots))[(*evt)->task]->append(e);

                if (e->exit > endtime)
                    endtime = e->exit;
                if (!stack.isEmpty())
                {
                    stack.top()->children.append(e);
                }
                for (QList<Event *>::Iterator child = bgn->children.begin();
                     child != bgn->children.end(); ++child)
                {
                    e->callees->append(*child);
                    (*child)->caller = e;
                }

                (*(trace->events))[(*evt)->task]->append(e);
            } // Handle this Event
        } // Loop Event
    } // Loop Event Lists
}

int CharmImporter::makeTasks()
{
    int taskid = 0;
    for (QMap<int, Chare *>::Iterator chare = chares->begin();
         chare != chares->end(); ++chare)
    {
        if (chare.value()->indices->size() > 1)
        {
            taskgroups->insert(chare.key(), new TaskGroup(chare.key(), chare.value()->name));
            for (QSet<QString>::Iterator index = chare.value()->indices->begin();
                 index != chare.value()->indices->end(); ++index)
            {
                Task * task = new Task(taskid, *index);
                taskgroups->value(chare.key())->tasks->append(taskid);
                tasks->insert(taskid, task);
                chare_to_task.insert(*index, taskid);
                taskid++;
            }
        }
    }
    return taskid;
}


void CharmImporter::parseLine(QString line, int my_pe)
{
    int index, mtype, entry, event, pe, id[4];
    long time, msglen, sendTime, recvTime, numpes, cpuStart, cpuEnd;

    std::cout << line.toStdString().c_str() << std::endl;
    QStringList lineList = line.split(" ");
    int rectype = lineList.at(0).toInt();
    if (rectype == CREATION || rectype == CREATION_BCAST)
    {
        // Some type of (multi-send)
        mtype = lineList.at(1).toInt();
        entry = lineList.at(2).toInt();
        time = lineList.at(3).toLong();
        event = lineList.at(4).toInt();
        pe = lineList.at(5).toInt(); // Should be my pe
        msglen = -1;
        index = 6;
        if (version >= 2.0)
        {
            msglen = lineList.at(5).toLong();
            index++;
        }
        if (version >= 5.0)
        {
            sendTime = lineList.at(index).toLong();
            index++;
        }
        if (rectype == CREATION_BCAST)
        {
            numpes = lineList.at(index).toInt();
        }

        CharmEvt * evt = new CharmEvt(SEND_FXN, time, pe, true);
        evt->chare = entries->value(entry)->chare;
        evt->entry = SEND_FXN;
        if (last)
            for (int i = 0; i < 4; i++)
                evt->chareIndex[i] = last->chareIndex[i];
        charm_events->at(pe)->append(evt);

        CharmEvt * send_end = new CharmEvt(SEND_FXN, time+1, pe, false);
        send_end->chare = entries->value(entry)->chare;
        send_end->entry = SEND_FXN;
        charm_events->at(pe)->append(send_end);
        if (last)
            for (int i = 0; i < 4; i++)
                send_end->chareIndex[i] = last->chareIndex[i];

        if (rectype == CREATION)
        {
            //std::cout << "Send on " << pe << " of " << entries->value(entry)->name.toStdString().c_str() << std::endl;
            //std::cout << "     Event: " << event << ", msg_type: " << mtype << std::endl;

            // Look for our message
            CharmMsg * msg = NULL;
            QList<CharmMsg *> * candidates = unmatched_msgs->at(pe)->value(event);
            if (candidates)
            {
                for (QList<CharmMsg *>::Iterator candidate = candidates->begin();
                     candidate != candidates->end(); ++candidate)
                {
                    if ((*candidate)->entry == entry)
                    {
                        msg = *candidate;
                        break;
                    }
                }
            }

            if (msg) // Found!
            {
                candidates->removeOne(msg);
            }
            else
            {
                msg = new CharmMsg(mtype, msglen, pe, entry, event, my_pe);
                if (!unmatched_msgs->at(pe)->contains(event))
                {
                    unmatched_msgs->at(pe)->insert(event, new QList<CharmMsg *>());
                }
                unmatched_msgs->at(pe)->value(event)->append(msg);
                messages->append(msg);
            }

            msg->sendtime = time;
            evt->charmmsg = msg;
        }
        else
        {
            /*for (int i = 0; i < numpes; i++)
            {
                CharmMsg * msg = new CharmMsg(mtype, msglen, pe, entry, event, my_pe);
                msg->sendtime = time;
                sends->append(msg);
                unmatched_sends->at(pe)->append(msg);
            }*/
            std::cout << "Bcast on " << pe << " of " << entries->value(entry)->name.toStdString().c_str() << std::endl;
            std::cout << "     Event: " << event << ", msg_type: " << mtype << std::endl;

        }

        /* By PE
        rawtrace->events->at(my_pe)->append(new EventRecord(my_pe,
                                                            time,
                                                            999998,
                                                            true));

        rawtrace->events->at(my_pe)->append(new EventRecord(my_pe,
                                                            time,
                                                            999998,
                                                            false));
                                                            */
    }
    else if (rectype == BEGIN_PROCESSING)
    {
        // A receive immediately followed by a function
        mtype = lineList.at(1).toInt();
        entry = lineList.at(2).toInt();
        time = lineList.at(3).toLong();
        event = lineList.at(4).toInt();
        pe = lineList.at(5).toInt(); // Should be the senders pe
        index = 6;
        msglen = -1;
        if (version >= 2.0)
        {
            msglen = lineList.at(index).toLong();
            index++;
        }
        if (version >= 4.0)
        {
            recvTime = lineList.at(index).toLong();
            index++;
            for (int i = 0; i < 3; i++)
            {
                id[i] = lineList.at(index).toInt();
                index++;
            }
        }
        if (version >= 7.0)
        {
            id[3] = lineList.at(index).toInt();
            index++;
        }
        if (version >= 6.5)
        {
            cpuStart = lineList.at(index).toLong();
            index++;
        }
        if (version >= 6.6)
        {
            // PerfCount stuff... skip for now.
        }

        CharmEvt * evt = new CharmEvt(entry, time, pe, true);
        evt->chare = entries->value(entry)->chare;
        evt->entry = entry;
        for (int i = 0; i < 4; i++)
            evt->chareIndex[i] = id[i];
        chares->value(evt->chare)->indices->insert(evt->indexToString());
        charm_events->at(pe)->append(evt);

        last = evt;



        /* By PE
        // Real Event
        rawtrace->events->at(my_pe)->append(new EventRecord(my_pe,
                                                            time,
                                                            entry,
                                                            true));

        // RECV
        rawtrace->events->at(my_pe)->append(new EventRecord(my_pe,
                                                            time,
                                                            999999,
                                                            true));
        rawtrace->events->at(my_pe)->append(new EventRecord(my_pe,
                                                            time,
                                                            999999,
                                                            false));
                                                            */

        CharmMsg * msg = NULL;
        if (pe > my_pe) // We get send later
        {
            msg = new CharmMsg(mtype, msglen, pe, entry, event, my_pe);
            if (!unmatched_msgs->at(pe)->contains(event))
            {
                unmatched_msgs->at(pe)->insert(event, new QList<CharmMsg *>());
            }
            unmatched_msgs->at(pe)->value(event)->append(msg);
            messages->append(msg);
        } else { // Send already exists
            QList<CharmMsg *> * candidates = unmatched_msgs->at(pe)->value(event);
            if (candidates) // May be missing some send events due to runtime collection
            {
                for (QList<CharmMsg *>::Iterator candidate = candidates->begin();
                     candidate != candidates->end(); ++candidate)
                {
                    if ((*candidate)->entry == entry)
                    {
                        msg = *candidate;
                        break;
                    }
                }
                candidates->removeOne(msg);
            }
        }
        if (msg)
        {
            // +1 to make sort properly
            evt = new CharmEvt(RECV_FXN, time+1, pe, true);
            evt->chare = entries->value(entry)->chare;
            evt->entry = RECV_FXN;
            for (int i = 0; i < 4; i++)
                evt->chareIndex[i] = id[i];
            charm_events->at(pe)->append(evt);

            CharmEvt * recv_end = new CharmEvt(RECV_FXN, time+2, pe, false);
            recv_end->chare = entries->value(entry)->chare;
            recv_end->entry = RECV_FXN;
            for (int i = 0; i < 4; i++)
                recv_end->chareIndex[i] = id[i];
            charm_events->at(pe)->append(recv_end);

            msg->recvtime = time;
            evt->charmmsg = msg;
        }

    }
    else if (rectype == END_PROCESSING)
    {
        // End of function
        mtype = lineList.at(1).toInt();
        entry = lineList.at(2).toInt();
        time = lineList.at(3).toLong();
        event = lineList.at(4).toInt();
        pe = lineList.at(5).toInt();
        index = 6;
        msglen = -1;
        if (version >= 2.0)
        {
            msglen = lineList.at(index).toLong();
            index++;
        }
        if (version >= 6.5)
        {
            cpuEnd = lineList.at(index).toLong();
            index++;
        }
        if (version >= 6.6)
        {
            // PerfCount stuff... skip for now.
        }

        CharmEvt * evt = new CharmEvt(entry, time, pe, false);
        evt->chare = entries->value(entry)->chare;
        evt->entry = entry;
        for (int i = 0; i < 4; i++)
            evt->chareIndex[i] = last->chareIndex[i];
        charm_events->at(pe)->append(evt);

        /* By PE
        rawtrace->events->at(my_pe)->append(new EventRecord(my_pe,
                                                            time,
                                                            entry,
                                                            false));
                                                            */
    }
    else if (rectype == MESSAGE_RECV) // just in case we ever find one
    {
        std::cout << "Message Recv!" << std::endl;
    }


}


bool CharmImporter::matchingMessages(CharmMsg * send, CharmMsg * recv)
{
    // Event is the unique ID of each message
    if (send->entry == recv->entry && send->event == recv->event
            && send->send_pe == recv->send_pe)
        return true;
    return false;
}

const int CharmImporter::SEND_FXN;
const int CharmImporter::RECV_FXN;

void CharmImporter::processDefinitions()
{
    functiongroups->insert(0, "MPI");
    functiongroups->insert(1, "Entry");

    functions->insert(SEND_FXN, new Function("Send", 0));
    functions->insert(RECV_FXN, new Function("Recv", 0));
    for (QMap<int, Entry *>::Iterator entry = entries->begin();
         entry != entries->end(); ++entry)
    {
        int id = entry.key();
        Entry * e = entry.value();

        functions->insert(id, new Function(e->name, 1));
    }
    entries->insert(SEND_FXN, new Entry(0, "Send", 0));
    entries->insert(RECV_FXN, new Entry(0, "Recv", 0));
}

// STS is never gzipped
void CharmImporter::readSts(QString dataFileName)
{
    std::ifstream stsfile(dataFileName.toStdString().c_str());
    std::string line;

    while (std::getline(stsfile, line))
    {
        QStringList lineList = QString::fromStdString(line).split(" ");
        if (lineList.at(0) == "ENTRY" && lineList.at(1) == "CHARE")
        {
            int len = lineList.length();
            QString name = lineList.at(3);
            for (int i = 4; i < len - 2; i++)
                name += " " + lineList.at(i);
            entries->insert(lineList.at(2).toInt(),
                            new Entry(lineList.at(len-2).toInt(), name,
                                      lineList.at(len-1).toInt()));
        }
        else if (lineList.at(0) == "CHARE")
        {
            chares->insert(lineList.at(1).toInt(),
                           new Chare(lineList.at(2)));
        }
        else if (lineList.at(0) == "VERSION")
        {
            version = lineList.at(1).toFloat();
        }
        else if (lineList.at(0) == "PROCESSORS")
        {
            processes = lineList.at(1).toInt();
        }
    }

    stsfile.close();
}


