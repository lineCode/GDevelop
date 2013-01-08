/** \file
 *  Game Develop
 *  2008-2012 Florian Rival (Florian.Rival@gmail.com)
 */
#if defined(GD_IDE_ONLY)

#include "CodeCompiler.h"
#include <SFML/System.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include "GDL/CommonTools.h"
#include "GDL/Scene.h"
#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/txtstrm.h>

using namespace std;

CodeCompiler *CodeCompiler::_singleton = NULL;
sf::Mutex CodeCompiler::openSaveDialogMutex;
const wxEventType CodeCompiler::refreshEventType = wxNewEventType();
const wxEventType CodeCompiler::processEndedEventType = wxNewEventType();

namespace {

/**
 * \brief Tool function to create a task which is empty.
 */
CodeCompilerTask ConstructEmptyTask()
{
    CodeCompilerTask task;
    task.emptyTask = true;
    return task;
}

}

void CodeCompiler::StartTheNextTask()
{
    //Check if there is a task to be made
    {
        sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

        if ( !currentTask.emptyTask )
        {
           //currentTask is not empty: Do it.
        }
        else
        {
            bool newTaskFound = false;
            for (unsigned int i = 0;i<pendingTasks.size();++i)
            {
                //Be sure that the task is not disabled
                if ( find(compilationDisallowed.begin(), compilationDisallowed.end(), pendingTasks[i].scene) == compilationDisallowed.end() )
                {
                    currentTask = pendingTasks[i];
                    pendingTasks.erase(pendingTasks.begin()+i);

                    newTaskFound = true;
                    break;
                }
            }
            if ( !newTaskFound ) //Bail out if no task can be made
            {
                if ( pendingTasks.empty() )
                    std::cout << "No more task to be processed." << std::endl;
                else
                    std::cout << "No more task to be processed ( But "+ToString(pendingTasks.size())+" disabled task(s) waiting for being enabled )." << std::endl;

                processLaunched = false;
                NotifyControls();
                return;
            }

        }
    }

    std::cout << "Processing task " << currentTask.userFriendlyName << "..." << std::endl;
    lastTaskFailed = false;
    NotifyControls();
    bool skip = false; //Set to true if the preworker of the task asked to relaunch the task later.

    if ( currentTask.preWork != boost::shared_ptr<CodeCompilerExtraWork>() )
    {
        std::cout << "Launching pre work..." << std::endl;
        bool result = currentTask.preWork->Execute();

        if ( !result )
        {
            std::cout << "Preworker execution failed, task skipped." << std::endl;
            skip = true;
        }
        else if ( currentTask.preWork->requestRelaunchCompilationLater )
        {
            std::cout << "Preworker asked to launch the task later" << std::endl;
            sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.
            pendingTasks.push_back(currentTask);
            pendingTasks.back().preWork->requestRelaunchCompilationLater = false;

            skip = true;
        }
    }

    if ( skip ) //The preworker asked to skip the task
    {
        currentTask = ConstructEmptyTask();
        NotifyControls();
        StartTheNextTask(); //Calling recursively StartTheNextTask() is not a problem: The function will be called until a process is launched
                            //or until all tasks are skipped
        return;
    }

    lastTaskMessages.clear();

    //Define compilation arguments for Clang.
    std::vector<std::string> args;
    args.push_back("-o "+currentTask.outputFile);
    args.push_back("-w");
    args.push_back("-B"+baseDir+"CppPlatform/MinGW32/bin");
    if ( currentTask.optimize ) args.push_back("-O1");

    if ( !currentTask.link ) //Generate argument for compiling a file
    {
        args.push_back("-include D:/Florian/Programmation/GameDevelop2/IDE/scripts/events.h");
        args.push_back("-c "+currentTask.inputFile);

        //Headers directories
        for (std::set<std::string>::const_iterator header = headersDirectories.begin();header != headersDirectories.end();++header)
            args.push_back((*header).c_str());

        //Additional headers
        args.push_back("-nostdinc++");
        std::vector<std::string> additionalHeadersArgs;
        for (unsigned int i = 0;i<currentTask.additionalHeaderDirectories.size();++i)
            additionalHeadersArgs.push_back("-I"+currentTask.additionalHeaderDirectories[i]);
        for (unsigned int i = 0;i<additionalHeadersArgs.size();++i)
            args.push_back(additionalHeadersArgs[i].c_str());

        if ( !currentTask.compilationForRuntime ) args.push_back("-DGD_IDE_ONLY");

        //GD library related defines.
        #if defined(WINDOWS)
        args.push_back("-DGD_CORE_API=__declspec(dllimport)");
        args.push_back("-DGD_API=__declspec(dllimport)");
        args.push_back("-DGD_EXTENSION_API=__declspec(dllimport)");
        #elif defined(LINUX)
        args.push_back("-DGD_CORE_API= ");
        args.push_back("-DGD_API= ");
        args.push_back("-DGD_EXTENSION_API= ");
        #elif defined(MAC)
        args.push_back("-DGD_CORE_API= ");
        args.push_back("-DGD_API= ");
        args.push_back("-DGD_EXTENSION_API= ");
        #endif

        //Other common defines.
        #if defined(RELEASE)
        args.push_back("-DRELEASE");
        args.push_back("-DNDEBUG");
        args.push_back("-DBOOST_DISABLE_ASSERTS");
        #elif defined(DEV)
        args.push_back("-DDEV");
        args.push_back("-DNDEBUG");
        args.push_back("-DBOOST_DISABLE_ASSERTS");
        #elif defined(DEBUG)
        args.push_back("-DDEBUG");
        #endif
    }
    else //Generate argument for linking files
    {
        args.push_back("-shared");

        args.push_back(currentTask.inputFile);

        //All the files to be linked
        for (unsigned int i = 0;i<currentTask.extraObjectFiles.size();++i)
            args.push_back(currentTask.extraObjectFiles[i]);

        //Libraries and libraries directories
        #if defined(WINDOWS)
        args.push_back("-L"+baseDir+"CppPlatform/MinGW32/lib/");
        #endif
        if ( !currentTask.compilationForRuntime )
        {
            args.push_back("-L"+baseDir);
            args.push_back("-L"+baseDir+"CppPlatform/Extensions/");
        }
        else
        {
            args.push_back("-L"+baseDir+"Runtime/");
            args.push_back("-L"+baseDir+"CppPlatform/Extensions/Runtime/");
        }

        args.push_back("-lgdl");
        args.push_back("-lstdc++");
        if ( !currentTask.compilationForRuntime ) args.push_back("-lGDCore");
        #if defined(RELEASE) || defined(DEV)
        args.push_back("-lsfml-audio");
        args.push_back("-lsfml-network");
        args.push_back("-lsfml-graphics");
        args.push_back("-lsfml-window");
        args.push_back("-lsfml-system");
        #elif defined(DEBUG)
        args.push_back("-lsfml-audio-d");
        args.push_back("-lsfml-network-d");
        args.push_back("-lsfml-graphics-d");
        args.push_back("-lsfml-window-d");
        args.push_back("-lsfml-system-d");
        #endif
        for (unsigned int i = 0;i<currentTask.extraLibFiles.size();++i)
            args.push_back("-l"+currentTask.extraLibFiles[i]);
    }

    std::string argsStr;
    for (unsigned int i = 0;i<args.size();++i) argsStr += args[i]+" ";

    //Launching the process
    std::cout << "Launching compiler process...\n";
    currentTaskProcess = new CodeCompilerProcess(this);
    wxExecute(baseDir+"CppPlatform/MinGW32/bin/g++.exe "+argsStr, wxEXEC_ASYNC, currentTaskProcess);

    //When the process ends, it will call ProcessEndedWork()...
}

CodeCompilerProcess::CodeCompilerProcess(wxEvtHandler * parent_) :
    wxProcess(0),
    parent(parent_)
{
    std::cout << "CodeCompilerProcess created." << std::endl;
}

void CodeCompilerProcess::OnTerminate( int pid, int status )
{
    std::cout << "CodeCompilerProcess terminated with status " << status << "." << std::endl;
    while ( HasInput() ) ;

    exitCode = status;
    wxCommandEvent processEndedEvent( CodeCompiler::processEndedEventType );
    if ( parent != NULL) wxPostEvent(parent, processEndedEvent);
}

void CodeCompiler::ProcessEndedWork(wxCommandEvent & event)
{
    //...This function is called when a CodeCompilerProcess ends its job.
    std::cout << "CodeCompiler notified that the current process ended work." << std::endl;

    // Create and execute the frontend to generate an LLVM bitcode module.
    bool compilationSucceeded = (currentTaskProcess->exitCode == 0);
    if (!compilationSucceeded)
    {
        std::cout << "Compilation failed with exit code " << currentTaskProcess->exitCode << ".\n";
        lastTaskFailed = true;
    }
    else
    {
        std::cout << "Compilation succeeded." << std::endl;
    }

    //Compilation ended, loading diagnostics
    {
        lastTaskMessages.clear();
        for (unsigned int i = 0;i<currentTaskProcess->output.size();++i)
            lastTaskMessages += currentTaskProcess->output[i]+"\n";

        for (unsigned int i = 0;i<currentTaskProcess->outputErrors.size();++i)
            lastTaskMessages += currentTaskProcess->outputErrors[i]+"\n";
    }

    //Now do post work and notify task has been done.
    {
        if (currentTask.postWork != boost::shared_ptr<CodeCompilerExtraWork>() )
        {
            std::cout << "Launching post task" << std::endl;
            currentTask.postWork->compilationSucceeded = compilationSucceeded;
            currentTask.postWork->Execute();

            if ( currentTask.postWork->requestRelaunchCompilationLater )
            {
                std::cout << "Postworker asked to launch again the task later" << std::endl;

                pendingTasks.push_back(currentTask);
                pendingTasks.back().postWork->requestRelaunchCompilationLater = false;
            }
        }

        std::cout << "Task ended." << std::endl;
        currentTask = ConstructEmptyTask();
        NotifyControls();
    }

    //Launch the next task ( even if there is no task to be done )
    delete currentTaskProcess;
    currentTaskProcess = NULL;
    StartTheNextTask();
}

void CodeCompiler::NotifyControls()
{
    wxCommandEvent refreshEvent( refreshEventType );
    for (std::set<wxEvtHandler*>::iterator it = notifiedControls.begin();it != notifiedControls.end();++it)
    {
        if ( (*it) != NULL) wxPostEvent((*it), refreshEvent);
    }
}
/* TODO
void CodeCompiler::SendCurrentThreadToGarbage()
{
    sf::Lock lock(garbageThreadsMutex); //Disallow modifying garbageThreads.
    std::cout << "Old thread (" << currentTaskThread.get() << ") sent to garbage." << std::endl;

    garbageThreads.push_back(currentTaskThread);
    livingGarbageThreadsCount++; //We increment livingGarbageThreadsCount as the thread sent to garbageThreads was alive ( i.e. : doing work )

    currentTaskThread = boost::shared_ptr<sf::Thread>();
    processLaunched = false;
}*/

void CodeCompiler::AddTask(CodeCompilerTask task)
{
    {
        sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

        //Check if an equivalent task is not waiting in the pending list
        for (unsigned int i = 0;i<pendingTasks.size();++i)
        {
            if ( task.IsSameTaskAs(pendingTasks[i]) ) return;
        }

        //If the task is equivalent to the current one, abort it.
        if ( processLaunched && task.IsSameTaskAs(currentTask) )
        {
            std::cout << "Task requested is equivalent to the current one (" << task.userFriendlyName << ")" << std::endl;

            /*if ( livingGarbageThreadsCount < maxGarbageThread ) TODO
            {
                SendCurrentProcessToGarbage();
            }
            else*/
            {
                pendingTasks.push_back(task);
                std::cout << "Max thread count reached, new pending task added (" << task.userFriendlyName << ")" << std::endl;
            }
        }
        else
        {
            pendingTasks.push_back(task);
            std::cout << "New pending task added (" << task.userFriendlyName << ")" << std::endl;
        }
    }

    if ( !processLaunched )
    {
        std::cout << "Launching new compilation run";
        processLaunched = true;
        StartTheNextTask();
    }
}

std::vector < CodeCompilerTask > CodeCompiler::GetCurrentTasks() const
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    std::vector < CodeCompilerTask > allTasks = pendingTasks;
    if (processLaunched) allTasks.insert(allTasks.begin(), currentTask);

    return allTasks;
}

bool CodeCompiler::HasTaskRelatedTo(Scene & scene) const
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    if ( processLaunched && currentTask.scene == &scene ) return true;

    for (unsigned int i = 0;i<pendingTasks.size();++i)
    {
        if ( pendingTasks[i].scene == &scene ) return true;
    }

    return false;
}

void CodeCompiler::EnableTaskRelatedTo(Scene & scene)
{
    bool mustLaunchCompilation = false;
    {
        sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

        std::cout << "Enabling tasks related to scene:" << scene.GetName() << endl;

        vector<Scene*>::iterator it = find(compilationDisallowed.begin(), compilationDisallowed.end(), &scene);
        if ( it != compilationDisallowed.end())
            compilationDisallowed.erase(it);

        mustLaunchCompilation = !pendingTasks.empty();
    }

    //Launch pending tasks if needed
    if ( !processLaunched && mustLaunchCompilation )
    {
        std::cout << "Launching compilation thread...";
        processLaunched = true;
        StartTheNextTask();
    }
}

void CodeCompiler::RemovePendingTasksRelatedTo(Scene & scene)
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    for (unsigned int i = 0;i<pendingTasks.size();)
    {
        if ( pendingTasks[i].scene == &scene )
            pendingTasks.erase(pendingTasks.begin()+i);
        else
            ++i;
    }

}

void CodeCompiler::DisableTaskRelatedTo(Scene & scene)
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    std::cout << "Disabling tasks related to scene:" << scene.GetName() << endl;

    vector<Scene*>::iterator it = find(compilationDisallowed.begin(), compilationDisallowed.end(), &scene);
    if ( it == compilationDisallowed.end())
        compilationDisallowed.push_back(&scene);
}

bool CodeCompiler::CompilationInProcess() const
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    return processLaunched;
}

void CodeCompiler::SetOutputDirectory(std::string outputDir_)
{
    outputDir = outputDir_;
    if ( outputDir.empty() || (outputDir[outputDir.length()-1] != '/' && outputDir[outputDir.length()-1] != '\\' ) )
        outputDir += "/";

    if (!wxDirExists(outputDir.c_str()))
        wxMkdir(outputDir);
}

void CodeCompiler::AddHeaderDirectory(const std::string & dir)
{
    wxFileName filename = wxFileName::FileName(dir);
    filename.MakeAbsolute(baseDir);

    headersDirectories.insert("-I"+ToString(filename.GetPath()));
}

void CodeCompiler::SetBaseDirectory(std::string baseDir_)
{
    std::string oldBaseDir = baseDir; //Remember the old base directory, see below
    baseDir = baseDir_;

    if ( baseDir.empty() || (baseDir[baseDir.length()-1] != '/' && baseDir[baseDir.length()-1] != '\\' ) )
        baseDir += "/"; //Normalize the path if needed

    std::vector<std::string> standardsIncludeDirs;
    #if defined(WINDOWS)
    standardsIncludeDirs.push_back("CppPlatform/MinGW32/include");
    standardsIncludeDirs.push_back("CppPlatform/MinGW32/lib/gcc/mingw32/4.5.2/include/c++");
    standardsIncludeDirs.push_back("CppPlatform/MinGW32/lib/gcc/mingw32/4.5.2/include/c++/mingw32");
    #elif defined(LINUX)
    standardsIncludeDirs.push_back("CppPlatform/include/linux/usr/include/i386-linux-gnu/");
    standardsIncludeDirs.push_back("CppPlatform/include/linux/usr/include");
    standardsIncludeDirs.push_back("CppPlatform/include/linux/usr/include/c++/4.6/");
    standardsIncludeDirs.push_back("CppPlatform/include/linux/usr/include/c++/4.6/i686-linux-gnu");
    standardsIncludeDirs.push_back("CppPlatform/include/linux/usr/include/c++/4.6/backward");
    #elif defined(MAC)
    #endif

    standardsIncludeDirs.push_back("CppPlatform/include/GDL");
    standardsIncludeDirs.push_back("CppPlatform/include/Core");
    standardsIncludeDirs.push_back("CppPlatform/include/boost");
    standardsIncludeDirs.push_back("CppPlatform/include/SFML/include");
    standardsIncludeDirs.push_back("CppPlatform/include/wxwidgets/include");
    standardsIncludeDirs.push_back("CppPlatform/include/wxwidgets/lib/gcc_dll/msw");
    standardsIncludeDirs.push_back("CppPlatform/Extensions/include");

    for (unsigned int i =0;i<standardsIncludeDirs.size();++i)
    {
        headersDirectories.erase("-I"+oldBaseDir+standardsIncludeDirs[i]); //Be sure to remove old include directories
        headersDirectories.insert("-I"+baseDir+standardsIncludeDirs[i]);
    }
}

void CodeCompiler::AllowMultithread(bool allow, unsigned int maxThread)
{
    /*if (!allow || maxThread == 1)
        maxGarbageThread = 0;
    else
        maxGarbageThread = maxThread-1;*/
}

CodeCompiler::CodeCompiler() :
    processLaunched(false),
    currentTask(ConstructEmptyTask()),
    currentTaskProcess(NULL),
    //maxGarbageThread(2),
    lastTaskFailed(false)
{
    Connect(wxID_ANY, processEndedEventType, (wxObjectEventFunction) (wxEventFunction) (wxCommandEventFunction) &CodeCompiler::ProcessEndedWork);
}

/**
 * Used to get output emitted by compilers.
 */
bool CodeCompilerProcess::HasInput()
{
    wxChar c;

    bool hasInput = false;
    // The original used wxTextInputStream to read a line at a time.  Fine, except when there was no \n, whereupon the thing would hang
    while ( IsInputAvailable() )
    {
        std::string line;
        do
        {
            c = GetInputStream()->GetC();
            if ( GetInputStream()->Eof() ) break; // Check we've not just overrun

            line += c;
            if ( c==wxT('\n') ) break; // If \n, break to print the line
        }
        while ( IsInputAvailable() ); // Unless \n, loop to get another char

        output.push_back(line); // Either there's a full line in 'line', or we've run out of input. Either way, print it

        hasInput = true;
    }

    while ( IsErrorAvailable() )
    {
        std::string line;
        do
        {
            c = GetErrorStream()->GetC();
            if ( GetErrorStream()->Eof() ) break; // Check we've not just overrun

            line += c;
            if ( c==wxT('\n') ) break; // If \n, break to print the line
        }
        while ( IsErrorAvailable() );                           // Unless \n, loop to get another char

        outputErrors.push_back(line); // Either there's a full line in 'line', or we've run out of input. Either way, print it

        hasInput = true;
    }

    return hasInput;
}

CodeCompilerExtraWork::CodeCompilerExtraWork() :
    requestRelaunchCompilationLater(false)
{
}
CodeCompilerExtraWork::~CodeCompilerExtraWork()
{
}

CodeCompiler::~CodeCompiler()
{
}

CodeCompiler * CodeCompiler::GetInstance()
{
    if ( NULL == _singleton )
        _singleton = new CodeCompiler;

    return ( static_cast<CodeCompiler*>( _singleton ) );
}

void CodeCompiler::DestroySingleton()
{
    if ( NULL != _singleton )
    {
        delete _singleton;
        _singleton = NULL;
    }
}
#endif

