#include <DB/Interpreters/InterpreterSelectQuery.h>
#include <DB/Interpreters/InterpreterInsertQuery.h>
#include <DB/Interpreters/InterpreterAlterQuery.h>
#include <DB/Storages/StorageBuffer.h>
#include <DB/Parsers/ASTInsertQuery.h>
#include <Poco/Ext/ThreadNumber.h>

#include <statdaemons/ext/range.hpp>


namespace DB
{


StoragePtr StorageBuffer::create(const std::string & name_, NamesAndTypesListPtr columns_, Context & context_,
	size_t num_shards_, const Thresholds & min_thresholds_, const Thresholds & max_thresholds_,
	const String & destination_database_, const String & destination_table_)
{
	return (new StorageBuffer{
		name_, columns_, context_, num_shards_, min_thresholds_, max_thresholds_, destination_database_, destination_table_})->thisPtr();
}


StorageBuffer::StorageBuffer(const std::string & name_, NamesAndTypesListPtr columns_, Context & context_,
	size_t num_shards_, const Thresholds & min_thresholds_, const Thresholds & max_thresholds_,
	const String & destination_database_, const String & destination_table_)
	: name(name_), columns(columns_), context(context_),
	num_shards(num_shards_), buffers(num_shards_),
	min_thresholds(min_thresholds_), max_thresholds(max_thresholds_),
	destination_database(destination_database_), destination_table(destination_table_),
	no_destination(destination_database.empty() && destination_table.empty()),
	log(&Logger::get("StorageBuffer (" + name + ")")),
	flush_thread([this] { flushThread(); })
{
}


/// Читает из одного буфера (из одного блока) под его mutex-ом.
class BufferBlockInputStream : public IProfilingBlockInputStream
{
public:
	BufferBlockInputStream(const Names & column_names_, StorageBuffer::Buffer & buffer_)
		: column_names(column_names_.begin(), column_names_.end()), buffer(buffer_) {}

	String getName() const { return "BufferBlockInputStream"; }

	String getID() const
	{
		std::stringstream res;
		res << "Buffer(" << &buffer;

		for (const auto & name : column_names)
			res << ", " << name;

		res << ")";
		return res.str();
	}

protected:
	Block readImpl()
	{
		Block res;

		if (has_been_read)
			return res;
		has_been_read = true;

		std::lock_guard<std::mutex> lock(buffer.mutex);

		if (!buffer.data)
			return res;

		for (size_t i = 0, size = buffer.data.columns(); i < size; ++i)
		{
			auto & col = buffer.data.unsafeGetByPosition(i);
			if (column_names.count(col.name))
				res.insert(col);
		}

		return res;
	}

private:
	NameSet column_names;
	StorageBuffer::Buffer & buffer;
	bool has_been_read = false;
};


BlockInputStreams StorageBuffer::read(
	const Names & column_names,
	ASTPtr query,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	size_t max_block_size,
	unsigned threads)
{
	processed_stage = QueryProcessingStage::FetchColumns;

	BlockInputStreams streams_from_dst;

	if (!no_destination)
		streams_from_dst = context.getTable(destination_database, destination_table)->read(
			column_names, query, settings, processed_stage, max_block_size, threads);

	BlockInputStreams streams_from_buffers;
	streams_from_buffers.reserve(num_shards);
	for (auto & buf : buffers)
		streams_from_buffers.push_back(new BufferBlockInputStream(column_names, buf));

	/** Если источники из таблицы были обработаны до какой-то не начальной стадии выполнения запроса,
	  * то тогда источники из буферов надо тоже обернуть в конвейер обработки до той же стадии.
	  */
	if (processed_stage > QueryProcessingStage::FetchColumns)
		for (auto & stream : streams_from_buffers)
			stream = InterpreterSelectQuery(query, context, processed_stage, 0, stream).execute();

	streams_from_dst.insert(streams_from_dst.end(), streams_from_buffers.begin(), streams_from_buffers.end());
	return streams_from_dst;
}


static void appendBlock(const Block & from, Block & to)
{
	size_t rows = from.rows();
	for (size_t column_no = 0, columns = to.columns(); column_no < columns; ++column_no)
	{
		const IColumn & col_from = *from.getByPosition(column_no).column.get();
		IColumn & col_to = *to.getByPosition(column_no).column.get();

		if (col_from.getName() != col_to.getName())
			throw Exception("Cannot append block to another: different type of columns at index " + toString(column_no)
				+ ". Block 1: " + from.dumpStructure() + ". Block 2: " + to.dumpStructure(), ErrorCodes::BLOCKS_HAS_DIFFERENT_STRUCTURE);

		for (size_t row_no = 0; row_no < rows; ++row_no)
			col_to.insertFrom(col_from, row_no);
	}
}


class BufferBlockOutputStream : public IBlockOutputStream
{
public:
	BufferBlockOutputStream(StorageBuffer & storage_) : storage(storage_) {}

	void write(const Block & block)
	{
		if (!block)
			return;

		size_t rows = block.rowsInFirstColumn();
		if (!rows)
			return;

		StoragePtr destination;
		if (!storage.no_destination)
		{
			destination = storage.context.tryGetTable(storage.destination_database, storage.destination_table);

			/// Проверяем структуру таблицы.
			try
			{
				destination->check(block, true);
			}
			catch (Exception & e)
			{
				e.addMessage("(when looking at destination table " + storage.destination_database + "." + storage.destination_table + ")");
				throw;
			}
		}

		size_t bytes = block.bytes();

		/// Если блок уже превышает максимальные ограничения, то пишем минуя буфер.
		if (rows > storage.max_thresholds.rows || bytes > storage.max_thresholds.bytes)
		{
			if (!storage.no_destination)
			{
				LOG_TRACE(storage.log, "Writing block with " << rows << " rows, " << bytes << " bytes directly.");
				storage.writeBlockToDestination(block, destination);
 			}
			return;
		}

		/// Распределяем нагрузку по шардам по номеру потока.
		const auto start_shard_num = Poco::ThreadNumber::get() % storage.num_shards;

		/// Перебираем буферы по кругу, пытаясь заблокировать mutex. Не более одного круга.
		auto shard_num = start_shard_num;
		size_t try_no = 0;
		for (; try_no != storage.num_shards; ++try_no)
		{
			std::unique_lock<std::mutex> lock(storage.buffers[shard_num].mutex, std::try_to_lock_t());
			if (lock.owns_lock())
			{
				insertIntoBuffer(block, storage.buffers[shard_num], std::move(lock));
				break;
			}

			++shard_num;
			if (shard_num == storage.num_shards)
				shard_num = 0;
		}

		/// Если так и не удалось ничего сразу заблокировать, то будем ждать на mutex-е.
		if (try_no == storage.num_shards)
			insertIntoBuffer(block, storage.buffers[start_shard_num], std::unique_lock<std::mutex>(storage.buffers[start_shard_num].mutex));
	}
private:
	StorageBuffer & storage;

	void insertIntoBuffer(const Block & block, StorageBuffer::Buffer & buffer, std::unique_lock<std::mutex> && lock)
	{
		if (!buffer.data)
		{
			buffer.first_write_time = time(0);
			buffer.data = block.cloneEmpty();
		}

		/// Если после вставки в буфер, ограничения будут превышены, то будем сбрасывать буфер.
		if (storage.checkThresholds(buffer, time(0), block.rowsInFirstColumn(), block.bytes()))
		{
			/// Вытащим из буфера блок, заменим буфер на пустой. После этого можно разблокировать mutex.
			Block block_to_write;
			buffer.data.swap(block_to_write);
			buffer.first_write_time = 0;
			lock.unlock();

			if (!storage.no_destination)
			{
				appendBlock(block, block_to_write);
				storage.writeBlockToDestination(block_to_write,
					storage.context.tryGetTable(storage.destination_database, storage.destination_table));
			}
		}
		else
			appendBlock(block, buffer.data);
	}
};


BlockOutputStreamPtr StorageBuffer::write(ASTPtr query)
{
	return new BufferBlockOutputStream(*this);
}


void StorageBuffer::shutdown()
{
	shutdown_event.set();

	if (flush_thread.joinable())
		flush_thread.join();

	optimize();
}


bool StorageBuffer::optimize()
{
	for (auto & buf : buffers)
		flushBuffer(buf, false);

	return true;
}


bool StorageBuffer::checkThresholds(Buffer & buffer, time_t current_time, size_t additional_rows, size_t additional_bytes)
{
	time_t time_passed = 0;
	if (buffer.first_write_time)
		time_passed = current_time - buffer.first_write_time;

	size_t rows = buffer.data.rowsInFirstColumn() + additional_rows;
	size_t bytes = buffer.data.bytes() + additional_bytes;

	bool res =
	       (time_passed > min_thresholds.time && rows > min_thresholds.rows && bytes > min_thresholds.bytes)
		|| (time_passed > max_thresholds.time || rows > max_thresholds.rows || bytes > max_thresholds.bytes);

	if (res)
		LOG_TRACE(log, "Flushing buffer with " << rows << " rows, " << bytes << " bytes, age " << time_passed << " seconds.");

	return res;
}


void StorageBuffer::flushBuffer(Buffer & buffer, bool check_thresholds)
{
	Block block_to_write;
	time_t current_time = check_thresholds ? time(0) : 0;

	/** Довольно много проблем из-за того, что хотим блокировать буфер лишь на короткое время.
	  * Под блокировкой, получаем из буфера блок, и заменяем в нём блок на новый пустой.
	  * Затем пытаемся записать полученный блок в подчинённую таблицу.
	  * Если этого не получилось - кладём данные обратно в буфер.
	  * Для этого также делается блокировка структуры таблицы.
	  * Замечание: может быть, стоит избавиться от такой сложности.
	  */
	{
		std::lock_guard<std::mutex> lock(buffer.mutex);

		if (check_thresholds && !checkThresholds(buffer, current_time))
			return;

		buffer.data.swap(block_to_write);
		buffer.first_write_time = 0;
	}

	if (no_destination)
		return;

	try
	{
		writeBlockToDestination(block_to_write, context.tryGetTable(destination_database, destination_table));
	}
	catch (...)
	{
		/// Возвращаем блок на место в буфер.

		std::lock_guard<std::mutex> lock(buffer.mutex);

		if (buffer.data)
		{
			/** Так как структура таблицы не изменилась, можно склеить два блока.
			  * Замечание: остаётся проблема - из-за того, что в разных попытках вставляются разные блоки,
			  *  теряется идемпотентность вставки в ReplicatedMergeTree.
			  */
			appendBlock(block_to_write, buffer.data);
			buffer.data.swap(block_to_write);
		}

		if (!buffer.first_write_time)
			buffer.first_write_time = current_time;

		/// Через некоторое время будет следующая попытка записать.
		throw;
	}
}


void StorageBuffer::writeBlockToDestination(const Block & block, StoragePtr table)
{
	if (no_destination || !block)
		return;

	if (!table)
	{
		LOG_ERROR(log, "Destination table " << destination_database << "." << destination_table << " doesn't exist. Block of data is discarded.");
		return;
	}

	ASTInsertQuery * insert = new ASTInsertQuery;
	ASTPtr ast_ptr = insert;

	insert->database = destination_database;
	insert->table = destination_table;

	/** Будем вставлять столбцы, являющиеся пересечением множества столбцов таблицы-буфера и подчинённой таблицы.
	  * Это позволит поддержать часть случаев (но не все), когда структура таблицы не совпадает.
	  */
	Block structure_of_destination_table = table->getSampleBlock();
	Names columns_intersection;
	columns_intersection.reserve(block.columns());
	for (size_t i : ext::range(0, structure_of_destination_table.columns()))
	{
		auto dst_col = structure_of_destination_table.unsafeGetByPosition(i);
		if (block.has(dst_col.name))
		{
			if (block.getByName(dst_col.name).type->getName() != dst_col.type->getName())
			{
				LOG_ERROR(log, "Destination table " << destination_database << "." << destination_table
					<< " have different type of column " << dst_col.name << ". Block of data is discarded.");
				return;
			}

			columns_intersection.push_back(dst_col.name);
		}
	}

	if (columns_intersection.empty())
	{
		LOG_ERROR(log, "Destination table " << destination_database << "." << destination_table << " have no common columns with block in buffer. Block of data is discarded.");
		return;
	}

	if (columns_intersection.size() != block.columns())
		LOG_WARNING(log, "Not all columns from block in buffer exist in destination table "
			<< destination_database << "." << destination_table << ". Some columns are discarded.");

	ASTExpressionList * list_of_columns = new ASTExpressionList;
	insert->columns = list_of_columns;
	list_of_columns->children.reserve(columns_intersection.size());
	for (const String & column : columns_intersection)
		list_of_columns->children.push_back(new ASTIdentifier(StringRange(), column, ASTIdentifier::Column));

	InterpreterInsertQuery interpreter{ast_ptr, context};

	auto block_io = interpreter.execute();
	block_io.out->writePrefix();
	block_io.out->write(block);
	block_io.out->writeSuffix();
}


void StorageBuffer::flushThread()
{
	do
	{
		try
		{
			optimize();
		}
		catch (...)
		{
			tryLogCurrentException(__PRETTY_FUNCTION__);
		}
	} while (!shutdown_event.tryWait(1000));
}


void StorageBuffer::alter(const AlterCommands & params, const String & database_name, const String & table_name, Context & context)
{
	auto lock = lockStructureForAlter();

	/// Чтобы не осталось блоков старой структуры.
	optimize();

	params.apply(*columns);
	InterpreterAlterQuery::updateMetadata(database_name, table_name, *columns, context);
}

}