#include "rpartition.h"
#include <iostream>

Partition::Partition()
    : events(new QMap<int, QList<Event *> *>),
      max_step(-1),
      max_global_step(-1),
      min_global_step(-1),
      dag_leap(-1),
      mark(false),
      parents(new QSet<Partition *>()),
      children(new QSet<Partition *>()),
      old_parents(new QSet<Partition *>()),
      old_children(new QSet<Partition *>()),
      mergable_parents(new QSet<Partition *>()),
      mergable_children(new QSet<Partition *>()),
      new_partition(NULL),
      tindex(-1),
      lowlink(-1),
      leapmark(false),
      group(new QSet<Partition *>()),
      gvid(""),
      gnome(NULL),
      gnome_type(0),
      cluster_processes(new QVector<ClusterProcess *>()),
      cluster_vectors(new QMap<int, QVector<long long int> *>()),
      cluster_step_starts(new QMap<int, int>()),
      free_recvs(NULL)
{
    group->insert(this); // We are always in our own group
}

Partition::~Partition()
{
    for (QMap<int, QList<Event *> *>::Iterator eitr = events->begin(); eitr != events->end(); ++eitr)
    {
        // Don't necessarily delete events as they are saved by merging
        delete eitr.value();
    }
    delete events;

    delete parents;
    delete children;
    delete old_parents;
    delete old_children;
    delete gnome;

    for (QMap<int, QVector<long long int> *>::Iterator itr =  cluster_vectors->begin();
         itr != cluster_vectors->end(); ++itr)
    {
        delete itr.value();
    }
    delete cluster_vectors;
    delete cluster_step_starts;
}

// Call when we are sure we want to delete events held in this partition
// (e.g. destroying the trace)
void Partition::deleteEvents()
{
    for (QMap<int, QList<Event *> *>::Iterator eitr = events->begin(); eitr != events->end(); ++eitr) {
        for (QList<Event *>::Iterator itr = (eitr.value())->begin(); itr != (eitr.value())->end(); ++itr) {
            delete *itr;
            *itr = NULL;
        }
    }
}

bool Partition::operator<(const Partition &partition)
{
    return min_global_step < partition.min_global_step;
}

bool Partition::operator>(const Partition &partition)
{
    return min_global_step > partition.min_global_step;
}

bool Partition::operator<=(const Partition &partition)
{
    return min_global_step <= partition.min_global_step;
}

bool Partition::operator>=(const Partition &partition)
{
    return min_global_step >= partition.min_global_step;
}

bool Partition::operator==(const Partition &partition)
{
    return min_global_step == partition.min_global_step;
}


// Does not order, just places them at the end
void Partition::addEvent(Event * e)
{
    if (events->contains(e->process))
    {
        ((*events)[e->process])->append(e);
    }
    else
    {
        (*events)[e->process] = new QList<Event *>();
        ((*events)[e->process])->append(e);
    }
}

void Partition::sortEvents(){
    for (QMap<int, QList<Event *> *>::Iterator event_list = events->begin(); event_list != events->end(); ++event_list) {
        qSort((event_list.value())->begin(), (event_list.value())->end(), dereferencedLessThan<Event>);
    }
}


// The minimum over all processes of the time difference between the last event
// in one partition and the first event in another, per process
unsigned long long int Partition::distance(Partition * other)
{
    unsigned long long int dist = ULLONG_MAX;
    for (QMap<int, QList<Event *> *>::Iterator event_list = events->begin(); event_list != events->end(); ++event_list)
    {
        if (other->events->contains(event_list.key()))
        {
            Event * my_last = (event_list.value())->last();
            Event * other_first = ((*(other->events))[event_list.key()])->first();
            if (other_first->enter > my_last->exit)
                dist = std::min(dist, other_first->enter - my_last->exit);
            else
            {
                Event * my_first = (event_list.value())->first();
                Event * other_last = ((*(other->events))[event_list.key()])->last();
                dist = std::min(dist, my_first->enter - other_last->exit);
            }
        }
    }
    return dist;
}

// Creates sets of mergable parents and children. If we don't consider collectives,
// this is the same as the parents and children. If we do consider them, it is a
// subset based on whether we are divided by a collective.
void Partition::setMergables(bool considerCollectives)
{
    mergable_parents->clear();
    mergable_children->clear();
    if (considerCollectives)
    {
        bool mergable;
        for (QSet<Partition *>::Iterator parent = parents->begin(); parent != parents->end(); ++parent)
        {
            mergable = true;
            for (QMap<int, QList<Event *> *>::Iterator event_list = (*parent)->events->begin();
                 event_list != (*parent)->events->end(); ++event_list)
            {
                if ((event_list.value())->last()->comm_next != (event_list.value())->last()->cc_next)
                    mergable = false;
            }
            if (mergable)
                mergable_parents->insert(*parent);
        }

        for (QSet<Partition *>::Iterator child = children->begin(); child != children->end(); ++child)
        {
            mergable = true;
            for (QMap<int, QList<Event *> *>::Iterator event_list = (*child)->events->begin();
                 event_list != (*child)->events->end(); ++event_list)
            {
                if ((event_list.value())->first()->comm_prev != (event_list.value())->first()->cc_prev)
                    mergable = false;
            }
            if (mergable)
                mergable_children->insert(*child);
        }
    }
    else
    {
        mergable_parents->unite(*parents);
        mergable_children->unite(*children);
    }
}

// Requires parent's dag leaps to be up to date
void Partition::calculate_dag_leap()
{
    dag_leap = 0;
    for (QSet<Partition *>::Iterator parent = parents->begin(); parent != parents->end(); ++parent)
        dag_leap = std::max(dag_leap, (*parent)->dag_leap + 1);

}

// Figure out step assignments for everything in partition
// TODO: Rewrite this so it is cleaner and closer to the paper
void Partition::step()
{
    free_recvs = new QList<Event *>();
    QList<Message *> * message_list = new QList<Message *>();

    // Setup strides
    int stride;
    Event * last_send;
    QList<Event *> * last_recvs;
    for (QMap<int, QList<Event *> *>::Iterator event_list = events->begin(); event_list != events->end(); ++event_list)
    {
        // For each process, start at the beginning
        last_recvs = new QList<Event *>();
        stride = 0;
        last_send = NULL;
        for (QList<Event *>::Iterator evt = (event_list.value())->begin(); evt != (event_list.value())->end(); ++evt)
        {
            // Initially set the step to the stride
            (*evt)->step = stride;
            // Determine whether the event is a send or a receive
            for (QVector<Message *>::Iterator msg = (*evt)->messages->begin(); msg != (*evt)->messages->end(); ++msg)
                if ((*evt) == (*msg)->sender)
                    message_list->append(*msg);
                else
                    (*evt)->is_recv = true;
            if ((*evt)->is_recv)
            {
                // Recv points back to its previous send
                (*evt)->last_send = last_send;

                // Store in last_recvs for now
                last_recvs->append(*evt);
            }
            else
            {
                // Sends increase the stride, reset the last_send, and take on the last_recvs
                ++stride;
                last_send = *evt;
                (*evt)->last_recvs = last_recvs;
                // Update recvs with their next send
                for (QList<Event *>::Iterator recv = (*evt)->last_recvs->begin(); recv != (*evt)->last_recvs->end(); ++recv)
                    (*recv)->next_send = *evt;
                // Restart last_recvs
                last_recvs = new QList<Event *>();
            }
        }
        // Leftover last_recvs are free
        if (last_recvs->size() > 0)
            free_recvs->append(last_recvs[0]);
    }

    // In message order, do the step_recvs
    qSort(message_list->begin(), message_list->end(), dereferencedLessThan<Message>);
    for (QList<Message *>::Iterator msg = message_list->begin(); msg != message_list->end(); ++msg)
        step_receive(*msg);

    // TODO: Fix Me: For some reason in pf3d, some of the recvs are not ending up ahead of their sends.
    // This must be an ordering issue. For now redoing the step_recv works. We may need to run multiple
    // times to make sure it is okay.
    for (QList<Message *>::Iterator msg = message_list->begin(); msg != message_list->end(); ++msg)
        step_receive(*msg);

    finalize_steps();

    delete message_list;
    delete free_recvs;
    delete last_recvs;
}

// In the message, make sure receive happens after send. If not,
// nudge to be after send and nudge everything else forwrad as well
// This is still done using steps in stride-space rather than logical steps,
// so many recvs are essentially in the same place
void Partition::step_receive(Message * msg)
{
    int previous_step;
    Event * previous_event, * next_event;
    if (msg->receiver->step <= msg->sender->step)
    {
        msg->receiver->step = msg->sender->step + 1;
        previous_step = msg->receiver->step;
        previous_event = msg->receiver;
        next_event = msg->receiver->comm_next;
        while (next_event && (next_event->partition == msg->receiver->partition))
        {
            if (previous_event->is_recv)
                next_event->step = previous_step;
            else
                next_event->step = previous_step + 1;

            previous_step = next_event->step;
            previous_event = next_event;
            next_event = next_event->comm_next;
        }
    }
}

// Here we expand the receives so they no longer occupy the same place as their following send
void Partition::restep()
{
    // Setup step dict
    QMap<int, QList<Event *> *> * step_dict = new QMap<int, QList<Event *> *>();
    for (QMap<int, QList<Event *> *>::Iterator event_list = events->begin(); event_list != events->end(); ++event_list)
    {
        for (QList<Event *>::Iterator evt = (event_list.value())->begin(); evt != (event_list.value())->end(); ++evt)
        {
            if (!(step_dict->contains((*evt)->step)))
                (*step_dict)[(*evt)->step] = new QList<Event *>();
            ((*step_dict)[(*evt)->step])->append(*evt);
        }
    }

    // Find the send steps/build the working dict
    QList<int> steps = step_dict->keys();
    qSort(steps);
    QMap<int, int> last_step = QMap<int, int>();
    QList<int> processes = events->keys();
    QMap<int, QMap<int, Event *> > working_step = QMap<int, QMap<int, Event*> >();
    for (QList<int>::Iterator process = processes.begin(); process != processes.end(); ++process)
    {
        last_step[*process] = -1;
        working_step[*process] = QMap<int, Event *>();
    }

    for (QMap<int, QList<Event *> *>::Iterator event_list = step_dict->begin(); event_list != step_dict->end(); ++event_list)
    {
        for (QList<Event *>::Iterator evt = (event_list.value())->begin(); evt != (event_list.value())->end(); ++evt)
        {
            if (!((*evt)->is_recv))
            {
                (working_step[(*evt)->process])[event_list.key()] = *evt;
                (*evt)->last_step = last_step[(*evt)->process];
                last_step[(*evt)->process] = event_list.key();
            }
        }
    }

    // Set the receives
    QMap<int, int> send_step = QMap<int, int>();
    send_step[-1] = -1;
    QList<Event *> sends = QList<Event *>();
    int current_step, submax, partition_step = 0;
    // For each step, we expand out working one process at a time putting final steps on.
    for (QList<int>::Iterator step = steps.begin(); step != steps.end(); ++step)
    {
        sends.clear();
        submax = partition_step;
        for (QList<int>::Iterator process = processes.begin(); process != processes.end(); ++process)
        {
            if ((working_step[*process]).contains(*step))
            {
                Event * evt = working_step[*process][*step];
                sends.append(evt); // Collect sends at this step
                current_step = send_step[evt->last_step] + 1; // Where we were at this step for this process

                // For all the recvs before the send, set the step, shifting forward if need be
                for (QList<Event *>::Iterator recv = evt->last_recvs->begin(); recv != evt->last_recvs->end(); ++recv)
                {
                    (*recv)->step = current_step;
                    // Did we break our rules? If so fix. We know the step would have gotten set for the
                    // sender by the time we're here so we can use 'step'
                    for (QVector<Message *>::Iterator msg = (*recv)->messages->begin(); msg != (*recv)->messages->end(); ++msg)
                        if ((*msg)->sender != (*recv) && (*msg)->sender->step >= current_step)
                        {
                            current_step = (*msg)->sender->step + 1;
                            (*recv)->step = current_step;
                        }

                    ++current_step;
                }
                submax = std::max(current_step, submax); // Sends get sent to maximum
            }
        }

        // Set the step for all the sends
        for (QList<Event *>::Iterator send = sends.begin(); send != sends.end(); ++send)
            (*send)->step = submax;
        send_step[*step] = submax;
    }

    // Handle free receives
    int prev_step;
    Event * next_recv;
    for (QList<Event *>::Iterator recv = free_recvs->begin(); recv != free_recvs->end(); ++recv)
    {
        // Find the step of that first free recv
        int step = 0;
        if ((*recv)->comm_prev && (*recv)->comm_prev->partition == (*recv)->partition)
            step = (*recv)->comm_prev->step + 1;
        for (QVector<Message *>::Iterator msg = (*recv)->messages->begin(); msg != (*recv)->messages->end(); ++msg)
            if ((*msg)->sender != (*recv))
                step = std::max(step, (*msg)->sender->step + 1);
        (*recv)->step = step;
        prev_step = step;

        // Set the steps of all the free recvs after it
        next_recv = (*recv)->comm_next;
        while (next_recv && next_recv->partition == (*recv)->partition)
        {
            step = prev_step;
            for (QVector<Message *>::Iterator msg = next_recv->messages->begin(); msg != next_recv->messages->end(); ++msg)
                if ((*msg)->sender != next_recv)
                    step = std::max(step, (*msg)->sender->step);
            next_recv->step = step + 1;
            prev_step = next_recv->step;
            next_recv = next_recv->comm_next;
        }
    }

    // delete step dict
    for (QMap<int, QList<Event *> *>::Iterator eitr = step_dict->begin(); eitr != step_dict->end(); ++eitr)
        delete eitr.value();
    delete step_dict;
}

// Find true logical steps, not just strides
void Partition::finalize_steps()
{
    // Unlike the Python version, this does nothing for waitall
    // This makes this function quite short in that it is:
    // restepping (expanding)
    // building the step dict <-- not needed for this lateness
    // determining the max step

    restep();

    // Find partition max_step -- going to be the last one of every list
    max_step = 0;
    for (QMap<int, QList<Event *> *>::Iterator event_list = events->begin(); event_list != events->end(); ++event_list)
    {
        max_step = std::max((event_list.value())->last()->step, max_step);
    }
}

void Partition::makeClusterVectors(QString metric)
{
    // Clean up old
    for (QMap<int, QVector<long long int> *>::Iterator itr =  cluster_vectors->begin();
         itr != cluster_vectors->end(); ++itr)
    {
        delete itr.value();
    }
    for (QVector<ClusterProcess *>::Iterator itr = cluster_processes->begin(); itr != cluster_processes->end(); ++itr)
    {
        delete *itr;
    }
    cluster_processes->clear();
    cluster_vectors->clear();
    cluster_step_starts->clear();


    // Create a ClusterProcess for each process and in each set metric_events so it fills in the missing
    // steps with the previous metric value.
    for (QMap<int, QList<Event *> *>::Iterator event_list = events->begin(); event_list != events->end(); ++event_list)
    {
        QVector<long long int> * metric_vector = new QVector<long long int>();
        (*cluster_vectors)[event_list.key()] = metric_vector;
        long long int last_value = 0;
        int last_step = (event_list.value())->at(0)->step;
        (*cluster_step_starts)[event_list.key()] = last_step;
        ClusterProcess * cp = new ClusterProcess(event_list.key(), last_step);
        cluster_processes->append(cp);
        for (QList<Event *>::Iterator evt = (event_list.value())->begin(); evt != (event_list.value())->end(); ++evt)
        {
            while ((*evt)->step > last_step + 2)
            {
                // Fill in the previous known value
                metric_vector->append(last_value);
                cp->metric_events->append(last_value);
                last_step += 2;
            }

            // Fill in our value
            last_step = (*evt)->step;
            last_value = (*evt)->getMetric(metric);
            metric_vector->append(last_value);
            cp->metric_events->append(last_value);
        }
        while (last_step <= max_global_step)
        {
            // We're out of steps but fill in the rest
            metric_vector->append(last_value);
            cp->metric_events->append(last_value);
            last_step += 2;
        }
    }
}

QString Partition::generate_process_string()
{
    QString ps = "";
    bool first = true;
    for (QMap<int, QList<Event *> *>::Iterator itr = events->begin(); itr != events->end(); ++itr)
    {
        if (!first)
            ps += ", ";
        else
            first = false;
        ps += QString::number(itr.key());
    }
    return ps;
}


int Partition::num_events()
{
    int count = 0;
    for (QMap<int, QList<Event *> *>::Iterator itr = events->begin(); itr != events->end(); ++itr)
        count += (*itr)->length();
    return count;
}

// Find what partition this one has been merged into
Partition * Partition::newest_partition()
{
    Partition * p = this;
    while (p != p->new_partition)
        p = p->new_partition;
    return p;
}
