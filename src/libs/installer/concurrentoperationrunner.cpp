/**************************************************************************
**
** Copyright (C) 2022 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Installer Framework.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
**************************************************************************/

#include "concurrentoperationrunner.h"

#include "errors.h"

#include <QtConcurrent>

using namespace QInstaller;

/*!
    \inmodule QtInstallerFramework
    \class QInstaller::ConcurrentOperationRunner
    \brief The ConcurrentOperationRunner class can be used to perform installer
           operations concurrently.

    The class accepts an operation list of any registered operation type. It can be
    used to execute the \c Backup, \c Perform, or \c Undo steps of the operations. The
    operations are run in the global thread pool, which by default limits the maximum
    number of threads to the ideal number of logical processor cores in the system.
*/

/*!
    \fn QInstaller::ConcurrentOperationRunner::finished()

    Emitted when the execution of all pooled operations is finished.
*/

/*!
    Constructs an operation runner with \a parent as the parent object.
*/
ConcurrentOperationRunner::ConcurrentOperationRunner(QObject *parent)
    : QObject(parent)
    , m_operations(nullptr)
    , m_type(Operation::OperationType::Perform)
{
}

/*!
    Constructs an operation runner with \a operations of type \a type to be performed,
    and \a parent as the parent object.
*/
ConcurrentOperationRunner::ConcurrentOperationRunner(OperationList *operations,
        const Operation::OperationType type, QObject *parent)
    : QObject(parent)
    , m_operations(operations)
    , m_type(type)
{
}

/*!
    Destroys the instance and releases resources.
*/
ConcurrentOperationRunner::~ConcurrentOperationRunner()
{
    qDeleteAll(m_operationWatchers);
}

/*!
    Sets the list of operations to be performed to \a operations.
*/
void ConcurrentOperationRunner::setOperations(OperationList *operations)
{
    m_operations = operations;
}

/*!
    Sets \a type of operations to be performed. This can be either
    \c Backup, \c Perform, or \c Undo.
*/
void ConcurrentOperationRunner::setType(const Operation::OperationType type)
{
    m_type = type;
}

/*!
    \internal

    Runs \a operation in mode of \a type. Returns \c true on success, \c false otherwise.
*/
static bool runOperation(Operation *const operation, const Operation::OperationType type)
{
    switch (type) {
    case Operation::Backup:
        operation->backup();
        return true;
    case Operation::Perform:
        return operation->performOperation();
    case Operation::Undo:
        return operation->undoOperation();
    default:
        Q_ASSERT(!"Unexpected operation type");
    }
    return false;
}

/*!
    Performs the current operations. Returns a hash of pointers to the performed operation
    objects and their results. The result is a boolean value.
*/
QHash<Operation *, bool> ConcurrentOperationRunner::run()
{
    reset();

    QEventLoop loop;
    for (auto &operation : qAsConst(*m_operations)) {
        auto futureWatcher = new QFutureWatcher<bool>();
        m_operationWatchers.insert(operation, futureWatcher);

        connect(futureWatcher, &QFutureWatcher<bool>::finished,
            this, &ConcurrentOperationRunner::onOperationfinished);

        futureWatcher->setFuture(QtConcurrent::run(&runOperation, operation, m_type));
    }

    if (!m_operationWatchers.isEmpty()) {
        connect(this, &ConcurrentOperationRunner::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }

    return m_results;
}

/*!
    Cancels operations pending for an asynchronous run.

    \note This does not stop already running operations, which
    should provide a separate mechanism for canceling.
*/
void ConcurrentOperationRunner::cancel()
{
    for (auto &watcher : m_operationWatchers)
        watcher->cancel();
}

/*!
    \internal

    Invoked when the execution of a single operation finishes. Adds the result
    of the operation to the return hash of \c ConcurrentOperationRunner::run().
*/
void ConcurrentOperationRunner::onOperationfinished()
{
    auto watcher = static_cast<QFutureWatcher<bool> *>(sender());

    Operation *op = m_operationWatchers.key(watcher);
    if (!watcher->isCanceled()) {
        try {
            // Catch transferred exceptions
            m_results.insert(op, watcher->future().result());
        } catch (const Error &e) {
            qCritical() << "Caught exception:" << e.message();
            m_results.insert(op, false);
        } catch (const QUnhandledException &) {
            qCritical() << "Caught unhandled exception in:" << Q_FUNC_INFO;
            m_results.insert(op, false);
        }
    } else {
        // Remember also operations canceled before execution
        m_results.insert(op, false);
    }

    delete m_operationWatchers.take(op);

    // All finished
    if (m_operationWatchers.isEmpty())
        emit finished();
}

/*!
    \internal

    Clears previous results and deletes remaining operation watchers.
*/
void ConcurrentOperationRunner::reset()
{
    qDeleteAll(m_operationWatchers);
    m_operationWatchers.clear();
    m_results.clear();
}
