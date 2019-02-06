#include "../Include/Manager.h"

#include "../Include/Assert.h"
#include "../Include/Logging.h"
#include "../Include/Fiber.h"

// TEMP
#include <thread>
#include <chrono>

void ManagerWorkerEntry(Manager* const Owner)
{
	JOBS_ASSERT(Owner, "Manager thread entry missing owner.");

	// Spin until the manager is ready.
	while (!Owner->Ready.load(/*std::memory_order_seq_cst*/)) [[unlikely]]  // #TODO: Memory order.
	{
		std::this_thread::yield();
	}

	// ThisFiber allows us to schedule other fibers.
	auto& Representation = Owner->Workers[Owner->GetThisThreadID()];

	JOBS_LOG(LogLevel::Log, "Worker Prep to Kick Off Fiber | ID: %i", Representation.GetID());

	// We don't have a fiber at this point, so grab an available fiber.
	auto NextFiberIndex{ Owner->GetAvailableFiber() };
	if (!Owner->IsValidID(NextFiberIndex))
	{
		// #TODO: If we failed to get a new fiber, allocate more.
	}

	Representation.FiberIndex = NextFiberIndex;

	JOBS_LOG(LogLevel::Log, "Worker acquired fiber.");

	// Schedule the fiber, which executes the work. We will resume here when the manager is shutting down.
	Owner->Fibers[NextFiberIndex].Schedule(Owner->Workers[Owner->GetThisThreadID()].GetThreadFiber());

	JOBS_LOG(LogLevel::Log, "Worker Shutdown | ID: %i", Representation.GetID());

	// Kill ourselves.
	delete &Representation.GetThreadFiber();
}

struct FiberData
{
	Manager* const Owner;
};

void ManagerFiberEntry(void* Data)
{
	JOBS_ASSERT(Data, "Manager fiber entry missing data.");

	auto FData = static_cast<FiberData*>(Data);

	while (FData->Owner->CanContinue())
	{
		auto Job{ std::move(FData->Owner->Dequeue()) };
		if (Job)
		{
			Job->Entry(Job->Data);
		}

		else
		{
			JOBS_LOG(LogLevel::Log, "No Job");

			// #TODO: Add a signal system.

			std::this_thread::sleep_for(std::chrono::milliseconds{ 1000 });
		}
	}

	// End of fiber lifetime, we are switching out to the worker thread to perform any final cleanup. We cannot be scheduled again beyond this point.
	auto& ActiveWorker{ FData->Owner->Workers[FData->Owner->GetThisThreadID()] };

	ActiveWorker.GetThreadFiber().Schedule(FData->Owner->Fibers[ActiveWorker.FiberIndex]);

	JOBS_ASSERT(false, "Dead fiber was rescheduled.");
}

Manager::Manager() {}

Manager::~Manager()
{
	Shutdown.store(true);  // #TODO: Memory order.

	// Wait for all of the workers to die before deleting the fiber data.
	for (auto& Worker : Workers)
	{
		Worker.GetHandle().join();
	}

	JOBS_LOG(LogLevel::Log, "Deleting Fiber Data.");

	delete Data;
}

void Manager::Initialize(std::size_t ThreadCount)
{
	JOBS_ASSERT(ThreadCount <= std::thread::hardware_concurrency(), "Job manager thread count should not exceed hardware concurrency.");

	Data = new FiberData{ this };

	for (auto Iter = 0; Iter < FiberCount; ++Iter)
	{
		Fibers.push_back(Fiber{ FiberStackSize, &ManagerFiberEntry, Data });
	}

	if (ThreadCount == 0)
	{
		ThreadCount = std::thread::hardware_concurrency();
	}

	Workers.reserve(ThreadCount);
	
	for (std::size_t Iter = 0; Iter < ThreadCount; ++Iter)
	{
		Workers.push_back(std::move(Worker{ this, Iter, &ManagerWorkerEntry }));
	}

	Shutdown.store(false);  // #TODO: Memory order.
	Ready.store(true, std::memory_order_seq_cst);  // #TODO: Memory order.
}

std::optional<Job> Manager::Dequeue()
{
	Job Result{};
	auto ThisThreadID{ GetThisThreadID() };

	if (Workers[ThisThreadID].GetJobQueue().try_dequeue(Result))
	{
		return Result;
	}

	else
	{
		// TEMP
		std::this_thread::sleep_for(std::chrono::milliseconds{ 200 });

		//JOBS_LOG(LogLevel::Log, "Stealing...");

		// Our queue is empty, time to steal.
		// #TODO: Implement a smart stealing algorithm.

		for (auto Iter = 0; Iter < Workers.size(); ++Iter)
		{
			if (Workers[(Iter + ThisThreadID) % Workers.size()].GetJobQueue().try_dequeue(Result))
			{
				return Result;
			}
		}
	}

	return std::nullopt;
}

std::size_t Manager::GetThisThreadID() const
{
	auto ThisID{ std::this_thread::get_id() };

	for (auto& Worker : Workers)
	{
		if (Worker.GetNativeID() == ThisID)
		{
			return Worker.GetID();
		}
	}

	return InvalidID;
}

bool Manager::IsValidID(std::size_t ID) const
{
	return ID != InvalidID;
}

bool Manager::CanContinue() const
{
	return !Shutdown.load();  // #TODO: Memory order.
}

std::size_t Manager::GetAvailableFiber()
{
	FiberPoolLock.Lock();

	// #TODO: Compare and swap operation instead of spinlock?
	
	for (auto Index = 0; Index < Fibers.size(); ++Index)
	{
		if (!Fibers[Index].Launched.load(std::memory_order_acquire))
		{
			Fibers[Index].Launched.store(true, std::memory_order_seq_cst);  // #TODO: Memory order.

			FiberPoolLock.Unlock();

			return Index;
		}
	}

	FiberPoolLock.Unlock();

	JOBS_LOG(LogLevel::Error, "No free fibers!");

	return InvalidID;
}