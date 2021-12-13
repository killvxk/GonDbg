#include "DebuggerCommands.h"
#include "Exceptions.h"

// TODO: Define the enum for the magic number 1 here used to indicate run another command. Also, make it the default command ret value

CommandResponse DebuggerCommands::help(const std::wstring&, Debugger& debugger)
{
	debugger.get_io_handler()->write(L"Supported commands:");
	for (const auto& command : get_commands())
	{
		debugger.get_io_handler()->write_formatted(L"%s", 
			command.first.c_str());
	}
	
	return CommandResponse::NoResponse;
}

CommandResponse DebuggerCommands::go(const std::wstring&, Debugger&)
{
    return CommandResponse::ContinueExecution;
}

CommandResponse DebuggerCommands::list_modules(const std::wstring&, Debugger& debugger)
{
	const auto& loaded_modules = debugger.get_symbol_finder().get_loaded_modules();
	for (const auto& m : loaded_modules)
	{
		debugger.get_io_handler()->write_formatted(L"0x%llX (0x%08lX): %s",
			static_cast<DWORD64>(m.get_image_base()),
			m.get_image_size(),
			m.get_image_name().c_str());
	}

	return CommandResponse::NoResponse;
}

CommandResponse DebuggerCommands::read_memory(const std::wstring& params, Debugger& debugger)
{
	const SIZE_T DEFAULT_READ_LENGTH = 0x40;
	RemotePointer address;
	SIZE_T length = DEFAULT_READ_LENGTH;

	int read_values = swscanf_s(params.c_str(), L"%Ix %Ix", &address, &length);

	switch (read_values)
	{
	case 1:
		__fallthrough;
	case 2:
		break;
	default:
		debugger.get_io_handler()->write(L"Invalid syntax. Usage: db <address> [L<length>]");
		return CommandResponse::NoResponse;
	}

	// TODO: Add support for invalid addresses (An exception). Maybe wrap this entire function with try
	std::vector<BYTE> data;
	try
	{
		data = debugger.get_process()->read_memory(address, length);
		debugger.get_io_handler()->write(debugger.get_io_handler()->format_bytes(data).c_str());
	}
	catch (const WinAPIException& e)
	{
		if (ERROR_PARTIAL_COPY == e.get_error())
		{
			// TODO: Do the same as WinDbg, and read whatever's possible (Will require to use read_page)
			debugger.get_io_handler()->write(L"Unmapped address");
		}
		else
		{
			throw;
		}
	}

	return CommandResponse::NoResponse;
}

CommandResponse DebuggerCommands::quit(const std::wstring&, Debugger&)
{
	throw DebuggingEnd();
}

CommandResponse DebuggerCommands::get_symbol_name(const std::wstring& params, Debugger& debugger)
{
	RemotePointer address;

	int read_values = swscanf_s(params.c_str(), L"%Ix", &address);
	switch (read_values)
	{
	case 1:
		break;
	default:
		debugger.get_io_handler()->write(L"Invalid syntax. Usage: x <address>");
		return CommandResponse::NoResponse;
	}

	debugger.get_io_handler()->write(debugger.get_symbol_finder().get_symbol(address));

	return CommandResponse::NoResponse;
}

class StepCallbackContext :
	public ICallbackContext
{
public:
	StepCallbackContext(Debugger* dbg, DWORD thread_id) :
		ICallbackContext(),
		m_debugger(dbg),
		m_original_thread_id(thread_id),
		m_triggered(false)
	{}
	virtual ~StepCallbackContext() = default;

	// NOTE: This callback keeps all suspended thread instead of just the thread not to resume, 
	// in case we'll receive a CreateThread event in between (By a remote process injection)
	// TODO: Find a more C++-ish way than a raw pointer
	Debugger* m_debugger;
	DWORD m_original_thread_id;
	bool m_triggered;
};

bool step_command_thread_creation_callback(const CreateThreadDebugEvent& debug_event, std::shared_ptr<ICallbackContext> context_raw)
{
	auto context = dynamic_cast<StepCallbackContext*>(context_raw.get());
	if (context->m_triggered)
	{
		return false;
	}

	// TODO: Preferably use the CreatedThread API. Sadly, ATM I don't pass it here :( And it's a waste of calls
	// Maybe adapt each callback to receive an appropriate object. In this case, CreatedThread
	if (-1 == SuspendThread(debug_event.get_handle()))
	{
		throw WinAPIException("SuspendThread failed");
	}

	return true;
}

bool step_command_exception_callback(const ExceptionDebugEvent& debug_event, std::shared_ptr<ICallbackContext> context_raw)
{
	auto context = dynamic_cast<StepCallbackContext*>(context_raw.get());

	if (debug_event.is_single_step())
	{
		for (auto& thread : context->m_debugger->get_threads())
		{
			if (thread.get_thread_id() != context->m_original_thread_id)
			{
				thread.resume();
			}
		}
		context->m_triggered = true;
		return false;
	}

	return true;
}

CommandResponse DebuggerCommands::step(const std::wstring&, Debugger& debugger)
{
	// TODO: Export an API to register to specific debug events (In this case, register_exception_event_handler)
	// The API will receive a callback that returns a bool. If true, the callback is kept inside the vector. Otherwise, it is removed.
	// TODO: Idea:
	// 1. Iterate over all process threads and set trace flag for all of them. (TODO: That requires implementing the thread maintaing)
	// 1.1. Alternative: Suspend all other threads. // TODO: Making a "context manager" for the freezing is both a good and a bad idea
	// 1.2. If a new thread is created before your trace happened, suspend it too
	// 2. Register exception callback that sets off trace flags for all threads, then returns false. Note: Even if it's a different exception, we don't care
	// Also register a thread creation callback that suspends new threads

	std::shared_ptr<StepCallbackContext> context(std::make_shared<StepCallbackContext>(&debugger, debugger.get_current_thread_id()));
	for (auto& thread : debugger.get_threads())
	{
		if (thread.get_thread_id() != debugger.get_current_thread_id())
		{
			thread.suspend();
		}
		else
		{
			thread.set_trap_flag();
		}
	}

	debugger.register_exception_callback(step_command_exception_callback, context);
	debugger.register_thread_creation_callback(step_command_thread_creation_callback, context);
	
	return CommandResponse::ContinueExecution;
}

CommandResponse DebuggerCommands::list_threads(const std::wstring&, Debugger& debugger)
{
	// TODO: Separate to running and dead threads?
	const auto& threads = debugger.get_threads();
	for (const auto& thread : threads)
	{
		debugger.get_io_handler()->write_formatted(L"0x%lX",
			thread.get_thread_id());
	}

	return CommandResponse::NoResponse;
}

std::vector<std::pair<std::wstring, CommandInterface>> DebuggerCommands::get_commands()
{
	return {
		std::make_pair(L"help", help),
		std::make_pair(L"g", go),
		std::make_pair(L"lm", list_modules),
		std::make_pair(L"db", read_memory),
		std::make_pair(L"exit", quit),
		std::make_pair(L"x", get_symbol_name),
		std::make_pair(L"lt", list_threads),
		std::make_pair(L"t", step)
	};
}
