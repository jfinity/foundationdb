/*
 * ReadWrite.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <boost/lexical_cast.hpp>

#include "fdbrpc/ContinuousSample.h"
#include "fdbclient/NativeAPI.h"
#include "fdbserver/TesterInterface.h"
#include "fdbserver/WorkerInterface.h"
#include "fdbserver/workloads/workloads.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "fdbserver/ClusterRecruitmentInterface.h"
#include "fdbclient/ReadYourWrites.h"
#include "flow/TDMetric.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

const int sampleSize = 10000;
static Future<Version> nextRV;
static Version lastRV = invalidVersion;

ACTOR static Future<Version> getNextRV(Database db) {
	state Transaction tr(db);
	loop {
		try {
			Version v = wait( tr.getReadVersion() );
			return v;
		} catch (Error& e) {
			wait( tr.onError(e) );
		}
	}
}
static Future<Version> getInconsistentReadVersion( Database const& db ) {
	if (!nextRV.isValid() || nextRV.isReady()) { // if no getNextRV() running
		if (nextRV.isValid()) lastRV = nextRV.get();
		nextRV = getNextRV( db );
	}
	if (lastRV == invalidVersion)
		return nextRV;
	else
		return lastRV;
}


DESCR struct TransactionSuccessMetric {
	int64_t totalLatency; // ns
	int64_t startLatency; // ns
	int64_t commitLatency; // ns
	int64_t retries; // count
}; 

DESCR struct TransactionFailureMetric {
	int64_t startLatency; // ns
	int64_t errorCode; // flow error code
};

DESCR struct ReadMetric {
	int64_t readLatency; // ns
};

struct ReadWriteWorkload : KVWorkload {
	int readsPerTransactionA, writesPerTransactionA;
	int readsPerTransactionB, writesPerTransactionB;
	int extraReadConflictRangesPerTransaction, extraWriteConflictRangesPerTransaction;
	double testDuration, transactionsPerSecond, alpha, warmingDelay, loadTime, maxInsertRate, debugInterval, debugTime;
	double metricsStart, metricsDuration, clientBegin;
	std::string valueString;

	bool dependentReads;
	bool enableReadLatencyLogging;
	double periodicLoggingInterval;
	bool cancelWorkersAtDuration;
	bool inconsistentReads;
	bool adjacentReads;
	bool adjacentWrites;
	bool rampUpLoad;
	int rampSweepCount;
	double hotKeyFraction, forceHotProbability; 
	bool rangeReads;
	bool useRYW;
	bool rampTransactionType;
	bool rampUpConcurrency;

	Int64MetricHandle totalReadsMetric;
	Int64MetricHandle totalRetriesMetric;
	EventMetricHandle<TransactionSuccessMetric> transactionSuccessMetric;
	EventMetricHandle<TransactionFailureMetric> transactionFailureMetric;
	EventMetricHandle<ReadMetric> readMetric;

	vector<Future<Void>> clients;
	PerfIntCounter aTransactions, bTransactions, retries;
	ContinuousSample<double> latencies, readLatencies, commitLatencies, GRVLatencies, fullReadLatencies;
	double readLatencyTotal; int readLatencyCount;

	vector<uint64_t> insertionCountsToMeasure;
	vector<pair<uint64_t, double> > ratesAtKeyCounts;

	vector<PerfMetric> periodicMetrics;

	bool doSetup;

	ReadWriteWorkload(WorkloadContext const& wcx)
		: KVWorkload(wcx),
		latencies( sampleSize ), readLatencies( sampleSize ), fullReadLatencies( sampleSize ), 
		commitLatencies( sampleSize ), GRVLatencies( sampleSize ), readLatencyTotal( 0 ), 
		readLatencyCount(0), loadTime(0.0), dependentReads(false), adjacentReads(false), adjacentWrites(false),
		clientBegin(0),	aTransactions("A Transactions"), bTransactions("B Transactions"), retries("Retries"),
		totalReadsMetric(LiteralStringRef("RWWorkload.TotalReads")),
		totalRetriesMetric(LiteralStringRef("RWWorkload.TotalRetries"))
	{
		transactionSuccessMetric.init(LiteralStringRef("RWWorkload.SuccessfulTransaction"));
		transactionFailureMetric.init(LiteralStringRef("RWWorkload.FailedTransaction"));
		readMetric.init(LiteralStringRef("RWWorkload.Read"));

		testDuration = getOption( options, LiteralStringRef("testDuration"), 10.0 );
		transactionsPerSecond = getOption( options, LiteralStringRef("transactionsPerSecond"), 5000.0 ) / clientCount;
		double allowedLatency = getOption(options, LiteralStringRef("allowedLatency"), 0.250);
		actorCount = ceil(transactionsPerSecond * allowedLatency);
		actorCount = getOption(options, LiteralStringRef("actorCountPerTester"), actorCount);

		readsPerTransactionA = getOption( options, LiteralStringRef("readsPerTransactionA"), 10 );
		writesPerTransactionA = getOption( options, LiteralStringRef("writesPerTransactionA"), 0 );
		readsPerTransactionB = getOption( options, LiteralStringRef("readsPerTransactionB"), 1 );
		writesPerTransactionB = getOption( options, LiteralStringRef("writesPerTransactionB"), 9 );
		alpha = getOption( options, LiteralStringRef("alpha"), 0.1 );

		extraReadConflictRangesPerTransaction = getOption(options, LiteralStringRef("extraReadConflictRangesPerTransaction"), 0);
		extraWriteConflictRangesPerTransaction = getOption(options, LiteralStringRef("extraWriteConflictRangesPerTransaction"), 0);
		
		valueString = std::string( maxValueBytes, '.' );
		if( nodePrefix > 0 ) {
			keyBytes += 16;
		}

		metricsStart = getOption( options, LiteralStringRef("metricsStart"), 0.0 );
		metricsDuration = getOption( options, LiteralStringRef("metricsDuration"), testDuration );
		if( getOption( options, LiteralStringRef("discardEdgeMeasurements"), true ) ) {
			// discardEdgeMeasurements keeps the metrics from the middle 3/4 of the test
			metricsStart += testDuration * 0.125;
			metricsDuration *= 0.75;
		}
		
		dependentReads = getOption( options, LiteralStringRef("dependentReads"), false );
		warmingDelay = getOption( options, LiteralStringRef("warmingDelay"), 0.0 );
		maxInsertRate = getOption( options, LiteralStringRef("maxInsertRate"), 1e12 );
		debugInterval = getOption( options, LiteralStringRef("debugInterval"), 0.0 );
		debugTime = getOption( options, LiteralStringRef("debugTime"), 0.0 );
		enableReadLatencyLogging = getOption( options, LiteralStringRef("enableReadLatencyLogging"), false );
		periodicLoggingInterval = getOption( options, LiteralStringRef("periodicLoggingInterval"), 5.0 );
		cancelWorkersAtDuration = getOption( options, LiteralStringRef("cancelWorkersAtDuration"), true );
		inconsistentReads = getOption( options, LiteralStringRef("inconsistentReads"), false );
		adjacentReads = getOption( options, LiteralStringRef("adjacentReads"), false ); 
		adjacentWrites = getOption(options, LiteralStringRef("adjacentWrites"), false);
		rampUpLoad = getOption(options, LiteralStringRef("rampUpLoad"), false);
		useRYW = getOption(options, LiteralStringRef("useRYW"), false);
		rampSweepCount = getOption(options, LiteralStringRef("rampSweepCount"), 1);
		rangeReads = getOption(options, LiteralStringRef("rangeReads"), false);
		rampTransactionType = getOption(options, LiteralStringRef("rampTransactionType"), false);
		rampUpConcurrency = getOption(options, LiteralStringRef("rampUpConcurrency"), false);
		doSetup = getOption(options, LiteralStringRef("setup"), true);

		if (rampUpConcurrency) ASSERT( rampSweepCount == 2 );  // Implementation is hard coded to ramp up and down

		// Validate that keyForIndex() is monotonic
		for (int i = 0; i < 30; i++) {
			int64_t a = g_random->randomInt64(0, nodeCount);
			int64_t b = g_random->randomInt64(0, nodeCount);
			if ( a > b ) {
				std::swap(a, b);
			}
			ASSERT(a <= b);
			ASSERT((keyForIndex(a, false) <= keyForIndex(b, false)));
		}

		vector<std::string> insertionCountsToMeasureString = getOption(options, LiteralStringRef("insertionCountsToMeasure"), vector<std::string>());
		for(int i = 0; i < insertionCountsToMeasureString.size(); i++)
		{
			try
			{
				uint64_t count = boost::lexical_cast<uint64_t>(insertionCountsToMeasureString[i]);
				insertionCountsToMeasure.push_back(count);
			}
			catch(...) {}
		}

		{
			// with P(hotTrafficFraction) an access is directed to one of a fraction 
			//   of hot keys, else it is directed to a disjoint set of cold keys
			hotKeyFraction = getOption( options, LiteralStringRef("hotKeyFraction"), 0.0 ); 
			double hotTrafficFraction = getOption( options, LiteralStringRef("hotTrafficFraction"), 0.0 );
			ASSERT(hotKeyFraction >= 0 && hotTrafficFraction <= 1);
			ASSERT(hotKeyFraction <= hotTrafficFraction); // hot keys should be actually hot!
			// p(Cold key) = (1-FHP) * (1-hkf)
			// p(Cold key) = (1-htf)
			// solving for FHP gives:
			forceHotProbability = (hotTrafficFraction-hotKeyFraction) / (1-hotKeyFraction);
		}
	}

	virtual std::string description() { return "ReadWrite"; }
	virtual Future<Void> setup( Database const& cx ) { return _setup( cx, this ); }
	virtual Future<Void> start( Database const& cx ) { return _start( cx, this ); }

	ACTOR static Future<bool> traceDumpWorkers( Reference<AsyncVar<ServerDBInfo>> db ) {
		try {
			loop {
				ErrorOr<vector<std::pair<WorkerInterface, ProcessClass>>> workerList = wait( db->get().clusterInterface.getWorkers.tryGetReply( GetWorkersRequest() ) );
				if( workerList.present() ) {
					std::vector<Future<ErrorOr<Void>>> dumpRequests;
					for( int i = 0; i < workerList.get().size(); i++)
						dumpRequests.push_back( workerList.get()[i].first.traceBatchDumpRequest.tryGetReply( TraceBatchDumpRequest() ) );
					wait( waitForAll( dumpRequests ) );
					return true;
				}
				wait( delay( 1.0 ) );
			}
		} catch( Error &e ) {
			TraceEvent(SevError, "FailedToDumpWorkers").error(e);
			throw;
		}
	}

	virtual Future<bool> check( Database const& cx ) { 
		clients.clear();

		if(!cancelWorkersAtDuration && now() < metricsStart + metricsDuration)
			metricsDuration = now() - metricsStart;

		g_traceBatch.dump();
		if( clientId == 0 )
			return traceDumpWorkers( dbInfo );
		else
			return true;
	}

	virtual void getMetrics( vector<PerfMetric>& m ) {
		double duration = metricsDuration;
		int reads = (aTransactions.getValue() * readsPerTransactionA) + (bTransactions.getValue() * readsPerTransactionB);
		int writes = (aTransactions.getValue() * writesPerTransactionA) + (bTransactions.getValue() * writesPerTransactionB);
		m.push_back( PerfMetric( "Measured Duration", duration, true ) );
		m.push_back( PerfMetric( "Transactions/sec", (aTransactions.getValue() + bTransactions.getValue()) / duration, false ) );
		m.push_back( PerfMetric( "Operations/sec", ( ( reads + writes ) / duration ), false ) );
		m.push_back( aTransactions.getMetric() );
		m.push_back( bTransactions.getMetric() );
		m.push_back( retries.getMetric() );
		m.push_back( PerfMetric( "Mean load time (seconds)", loadTime, true ) );
		m.push_back( PerfMetric( "Read rows", reads, false ) );
		m.push_back( PerfMetric( "Write rows", writes, false ) );

		if(!rampUpLoad) {
			m.push_back(PerfMetric("Mean Latency (ms)", 1000 * latencies.mean(), true));
			m.push_back(PerfMetric("Median Latency (ms, averaged)", 1000 * latencies.median(), true));
			m.push_back(PerfMetric("90% Latency (ms, averaged)", 1000 * latencies.percentile(0.90), true));
			m.push_back(PerfMetric("98% Latency (ms, averaged)", 1000 * latencies.percentile(0.98), true));
			m.push_back(PerfMetric("Max Latency (ms, averaged)", 1000 * latencies.max(), true));

			m.push_back(PerfMetric("Mean Row Read Latency (ms)", 1000 * readLatencies.mean(), true));
			m.push_back(PerfMetric("Median Row Read Latency (ms, averaged)", 1000 * readLatencies.median(), true));
			m.push_back(PerfMetric("Max Row Read Latency (ms, averaged)", 1000 * readLatencies.max(), true));

			m.push_back(PerfMetric("Mean Total Read Latency (ms)", 1000 * fullReadLatencies.mean(), true));
			m.push_back(PerfMetric("Median Total Read Latency (ms, averaged)", 1000 * fullReadLatencies.median(), true));
			m.push_back(PerfMetric("Max Total Latency (ms, averaged)", 1000 * fullReadLatencies.max(), true));

			m.push_back(PerfMetric("Mean GRV Latency (ms)", 1000 * GRVLatencies.mean(), true));
			m.push_back(PerfMetric("Median GRV Latency (ms, averaged)", 1000 * GRVLatencies.median(), true));
			m.push_back(PerfMetric("Max GRV Latency (ms, averaged)", 1000 * GRVLatencies.max(), true));

			m.push_back(PerfMetric("Mean Commit Latency (ms)", 1000 * commitLatencies.mean(), true));
			m.push_back(PerfMetric("Median Commit Latency (ms, averaged)", 1000 * commitLatencies.median(), true));
			m.push_back(PerfMetric("Max Commit Latency (ms, averaged)", 1000 * commitLatencies.max(), true));
		}

		m.push_back( PerfMetric( "Read rows/sec", reads / duration, false ) );
		m.push_back( PerfMetric( "Write rows/sec", writes / duration, false ) );
		m.push_back( PerfMetric( "Bytes read/sec", (reads * (keyBytes + (minValueBytes+maxValueBytes)*0.5)) / duration, false ) );
		m.push_back( PerfMetric( "Bytes written/sec", (writes * (keyBytes + (minValueBytes+maxValueBytes)*0.5)) / duration, false ) );
		m.insert(m.end(), periodicMetrics.begin(), periodicMetrics.end());

		vector<pair<uint64_t, double> >::iterator ratesItr = ratesAtKeyCounts.begin();
		for(; ratesItr != ratesAtKeyCounts.end(); ratesItr++)
			m.push_back(PerfMetric(format("%ld keys imported bytes/sec", ratesItr->first), ratesItr->second, false));
	}

	Value randomValue() { return StringRef( (uint8_t*)valueString.c_str(), g_random->randomInt(minValueBytes, maxValueBytes+1) );	}

	Standalone<KeyValueRef> operator()( uint64_t n ) {
		return KeyValueRef( keyForIndex( n, false ), randomValue() );
	} 

	ACTOR static Future<Void> tracePeriodically( ReadWriteWorkload *self ) {
		state double start = now();
		state double elapsed = 0.0;
		state int64_t last_ops = 0;

		loop {
			elapsed += self->periodicLoggingInterval;
			wait( delayUntil(start + elapsed) );

			TraceEvent("RW_RowReadLatency").detail("Mean", self->readLatencies.mean()).detail("Median", self->readLatencies.median()).detail("Percentile5", self->readLatencies.percentile(.05)).detail("Percentile95", self->readLatencies.percentile(.95)).detail("Count", self->readLatencyCount).detail("Elapsed", elapsed);
			TraceEvent("RW_GRVLatency").detail("Mean", self->GRVLatencies.mean()).detail("Median", self->GRVLatencies.median()).detail("Percentile5", self->GRVLatencies.percentile(.05)).detail("Percentile95", self->GRVLatencies.percentile(.95));
			TraceEvent("RW_CommitLatency").detail("Mean", self->commitLatencies.mean()).detail("Median", self->commitLatencies.median()).detail("Percentile5", self->commitLatencies.percentile(.05)).detail("Percentile95", self->commitLatencies.percentile(.95));
			TraceEvent("RW_TotalLatency").detail("Mean", self->latencies.mean()).detail("Median", self->latencies.median()).detail("Percentile5", self->latencies.percentile(.05)).detail("Percentile95", self->latencies.percentile(.95));

			int64_t ops = (self->aTransactions.getValue() * (self->readsPerTransactionA+self->writesPerTransactionA)) + 
						  (self->bTransactions.getValue() * (self->readsPerTransactionB+self->writesPerTransactionB));
			bool recordBegin = self->shouldRecord( std::max( now() - self->periodicLoggingInterval, self->clientBegin ) );
			bool recordEnd   = self->shouldRecord( now() );
			if( recordBegin && recordEnd ) {
				std::string ts = format("T=%04.0fs:", elapsed);
				self->periodicMetrics.push_back( PerfMetric( ts + "Operations/sec", (ops-last_ops)/self->periodicLoggingInterval, false ) );

				//if(self->rampUpLoad) {
					self->periodicMetrics.push_back(PerfMetric(ts + "Mean Latency (ms)", 1000 * self->latencies.mean(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "Median Latency (ms, averaged)", 1000 * self->latencies.median(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "5% Latency (ms, averaged)", 1000 * self->latencies.percentile(.05), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "95% Latency (ms, averaged)", 1000 * self->latencies.percentile(.95), true));

					self->periodicMetrics.push_back(PerfMetric(ts + "Mean Row Read Latency (ms)", 1000 * self->readLatencies.mean(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "Median Row Read Latency (ms, averaged)", 1000 * self->readLatencies.median(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "5% Row Read Latency (ms, averaged)", 1000 * self->readLatencies.percentile(.05), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "95% Row Read Latency (ms, averaged)", 1000 * self->readLatencies.percentile(.95), true));

					self->periodicMetrics.push_back(PerfMetric(ts + "Mean Total Read Latency (ms)", 1000 * self->fullReadLatencies.mean(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "Median Total Read Latency (ms, averaged)", 1000 * self->fullReadLatencies.median(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "5% Total Read Latency (ms, averaged)", 1000 * self->fullReadLatencies.percentile(.05), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "95% Total Read Latency (ms, averaged)", 1000 * self->fullReadLatencies.percentile(.95), true));

					self->periodicMetrics.push_back(PerfMetric(ts + "Mean GRV Latency (ms)", 1000 * self->GRVLatencies.mean(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "Median GRV Latency (ms, averaged)", 1000 * self->GRVLatencies.median(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "5% GRV Latency (ms, averaged)", 1000 * self->GRVLatencies.percentile(.05), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "95% GRV Latency (ms, averaged)", 1000 * self->GRVLatencies.percentile(.95), true));

					self->periodicMetrics.push_back(PerfMetric(ts + "Mean Commit Latency (ms)", 1000 * self->commitLatencies.mean(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "Median Commit Latency (ms, averaged)", 1000 * self->commitLatencies.median(), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "5% Commit Latency (ms, averaged)", 1000 * self->commitLatencies.percentile(.05), true));
					self->periodicMetrics.push_back(PerfMetric(ts + "95% Commit Latency (ms, averaged)", 1000 * self->commitLatencies.percentile(.95), true));
				//}

				self->periodicMetrics.push_back(PerfMetric(ts + "Max Latency (ms, averaged)", 1000 * self->latencies.max(), true));
				self->periodicMetrics.push_back(PerfMetric(ts + "Max Row Read Latency (ms, averaged)", 1000 * self->readLatencies.max(), true));
				self->periodicMetrics.push_back(PerfMetric(ts + "Max Total Read Latency (ms, averaged)", 1000 * self->fullReadLatencies.max(), true));
				self->periodicMetrics.push_back(PerfMetric(ts + "Max GRV Latency (ms, averaged)", 1000 * self->GRVLatencies.max(), true));
				self->periodicMetrics.push_back(PerfMetric(ts + "Max Commit Latency (ms, averaged)", 1000 * self->commitLatencies.max(), true));
			}
			last_ops = ops;

			//if(self->rampUpLoad) {
				self->latencies.clear();
				self->readLatencies.clear();
				self->fullReadLatencies.clear();
				self->GRVLatencies.clear();
				self->commitLatencies.clear();
			//}

			self->readLatencyTotal = 0.0;
			self->readLatencyCount = 0;
		}
	}

	ACTOR static Future<Void> logLatency( Future<Optional<Value>> f, ContinuousSample<double> *latencies, double* totalLatency, int* latencyCount, EventMetricHandle<ReadMetric> readMetric, bool shouldRecord ) {
		state double readBegin = now();
		Optional<Value> value = wait( f );

		double latency = now() - readBegin;
		readMetric->readLatency = latency * 1e9;
		readMetric->log();

		if( shouldRecord ) {
			*totalLatency += latency; ++*latencyCount;
			latencies->addSample( latency );
		}
		return Void();
	}

	ACTOR static Future<Void> logLatency(Future<Standalone<RangeResultRef>> f, ContinuousSample<double> *latencies, double* totalLatency, int* latencyCount, EventMetricHandle<ReadMetric> readMetric, bool shouldRecord) {
		state double readBegin = now();
		Standalone<RangeResultRef> value = wait(f);

		double latency = now() - readBegin;
		readMetric->readLatency = latency * 1e9;
		readMetric->log();

		if (shouldRecord) {
			*totalLatency += latency; ++*latencyCount;
			latencies->addSample(latency);
		}
		return Void();
	}

	ACTOR template <class Trans>
	Future<Void> readOp(Trans *tr, std::vector<int64_t> keys, ReadWriteWorkload *self, bool shouldRecord) {
		if( !keys.size() )
			return Void();
		if (!self->dependentReads){
			std::vector<Future<Void>> readers;
			if (self->rangeReads) {
				for (int op = 0; op < keys.size(); op++) {
					++self->totalReadsMetric;
					readers.push_back(logLatency(tr->getRange(KeyRangeRef(self->keyForIndex(keys[op]), Key(strinc(self->keyForIndex(keys[op])))), GetRangeLimits(-1, 80000)), &self->readLatencies, &self->readLatencyTotal, &self->readLatencyCount, self->readMetric, shouldRecord));
				}
			}
			else {
				for (int op = 0; op < keys.size(); op++) {
					++self->totalReadsMetric;
					readers.push_back(logLatency(tr->get(self->keyForIndex(keys[op])), &self->readLatencies, &self->readLatencyTotal, &self->readLatencyCount, self->readMetric, shouldRecord));
				}
			}
			wait( waitForAll( readers ) );
		} else {
			state int op;
			for(op = 0; op < keys.size(); op++ ) {
				++self->totalReadsMetric;
				wait( logLatency( tr->get( self->keyForIndex( keys[op] ) ), &self->readLatencies, &self->readLatencyTotal, &self->readLatencyCount, self->readMetric, shouldRecord) );
			}
		}
		return Void();
	}

	ACTOR Future<Void> _setup( Database cx, ReadWriteWorkload *self ) {
		if(!self->doSetup)
			return Void();

		state Promise<double> loadTime;
		state Promise<vector<pair<uint64_t, double> > > ratesAtKeyCounts;

		wait( bulkSetup( cx, self, self->nodeCount, loadTime, self->insertionCountsToMeasure.empty(), self->warmingDelay, self->maxInsertRate, 
								  self->insertionCountsToMeasure, ratesAtKeyCounts ) );

		self->loadTime = loadTime.getFuture().get();
		self->ratesAtKeyCounts = ratesAtKeyCounts.getFuture().get();

		return Void();
	}

	ACTOR Future<Void> _start( Database cx, ReadWriteWorkload *self ) {
		// Read one record from the database to warm the cache of keyServers 
		state std::vector<int64_t> keys;
		keys.push_back( g_random->randomInt64(0, self->nodeCount) );
		state double startTime = now();
		loop {
			state Transaction tr(cx);
			try {
				wait( self->readOp( &tr, keys, self, false ) );
				wait( tr.warmRange( cx, allKeys ) );
				break;
			} catch( Error& e ) {
				wait( tr.onError( e ) );
			}
		}

		wait( delay( std::max(0.1, 1.0 - (now() - startTime) ) ) );

		vector<Future<Void>> clients;
		if(self->enableReadLatencyLogging)
			clients.push_back(tracePeriodically(self));

		self->clientBegin = now();
		for(int c = 0; c < self->actorCount; c++) {
			Future<Void> worker;
			if (self->useRYW)
				worker = self->randomReadWriteClient<ReadYourWritesTransaction>(cx, self, self->actorCount / self->transactionsPerSecond, c);
			else
				worker = self->randomReadWriteClient<Transaction>(cx, self, self->actorCount / self->transactionsPerSecond, c);
			clients.push_back(worker);
		}

		if (!self->cancelWorkersAtDuration) self->clients = clients; // Don't cancel them until check()

		wait( self->cancelWorkersAtDuration ? timeout( waitForAll( clients ), self->testDuration, Void() ) : delay( self->testDuration ) );
		return Void();
	}

	bool shouldRecord() {
		return shouldRecord( now() );
	}
	
	bool shouldRecord( double checkTime ) {
		double timeSinceStart = checkTime - clientBegin;
		return timeSinceStart >= metricsStart && timeSinceStart < ( metricsStart + metricsDuration );
	}

	int64_t getRandomKey(uint64_t nodeCount){
		if (forceHotProbability && g_random->random01() < forceHotProbability)
			return g_random->randomInt64( 0, nodeCount*hotKeyFraction) / hotKeyFraction; // spread hot keys over keyspace
		else
			return g_random->randomInt64( 0, nodeCount );
	}

	double sweepAlpha(double startTime) {
		double sweepDuration = testDuration / rampSweepCount;
		double numSweeps = (now() - startTime) / sweepDuration;
		int currentSweep = (int)numSweeps;
		double alpha = numSweeps - currentSweep;
		if (currentSweep%2) alpha = 1 - alpha;
		return alpha;
	}

	ACTOR template <class Trans>
	Future<Void> randomReadWriteClient( Database cx, ReadWriteWorkload *self, double delay, int clientIndex ) {
		state double startTime = now();
		state double lastTime = now();
		state double GRVStartTime;
		state UID debugID;

		if (self->rampUpConcurrency) {
			wait( ::delay( self->testDuration/2 * (double(clientIndex) / self->actorCount + double(self->clientId) / self->clientCount / self->actorCount) ) );
			TraceEvent("ClientStarting").detail("ActorIndex", clientIndex).detail("ClientIndex", self->clientId).detail("NumActors", clientIndex*self->clientCount + self->clientId + 1);
		}

		loop {
			wait( poisson( &lastTime, delay ) );

			if (self->rampUpConcurrency) {
				if (now() - startTime >= self->testDuration/2 * (2 - (double(clientIndex) / self->actorCount + double(self->clientId) / self->clientCount / self->actorCount))) {
					TraceEvent("ClientStopping").detail("ActorIndex", clientIndex).detail("ClientIndex", self->clientId)
						.detail("NumActors", clientIndex*self->clientCount + self->clientId);
					wait(Never());
				}
			}

			if(!self->rampUpLoad || g_random->random01() < self->sweepAlpha(startTime))
			{
				state double tstart = now();
				state bool aTransaction = g_random->random01() > (self->rampTransactionType ? self->sweepAlpha(startTime) : self->alpha);

				state vector<int64_t> keys;
				state vector<Value> values;
				state vector<KeyRange> extra_ranges;
				int reads = aTransaction ? self->readsPerTransactionA : self->readsPerTransactionB;
				state int writes = aTransaction ? self->writesPerTransactionA : self->writesPerTransactionB;
				state int extra_read_conflict_ranges = writes ? self->extraReadConflictRangesPerTransaction : 0;
				state int extra_write_conflict_ranges = writes ? self->extraWriteConflictRangesPerTransaction : 0;
				if(!self->adjacentReads) {
					for(int op = 0; op < reads; op++)
						keys.push_back(self->getRandomKey(self->nodeCount));
				}
				else {
					int startKey = self->getRandomKey(self->nodeCount - reads);
					for(int op = 0; op < reads; op++)
						keys.push_back(startKey + op);
				}

				for (int op = 0; op<writes; op++)
					values.push_back(self->randomValue());

				for (int op = 0; op<extra_read_conflict_ranges + extra_write_conflict_ranges; op++)
					extra_ranges.push_back(singleKeyRange( g_random->randomUniqueID().toString() ));

				state Trans tr(cx);
				if(tstart - self->clientBegin > self->debugTime && tstart - self->clientBegin <= self->debugTime + self->debugInterval) {
					debugID = g_random->randomUniqueID();
					tr.debugTransaction(debugID);
					g_traceBatch.addEvent("TransactionDebug", debugID.first(), "ReadWrite.randomReadWriteClient.Before");
				}
				else {
					debugID = UID();
				}

				self->transactionSuccessMetric->retries = 0;
				self->transactionSuccessMetric->commitLatency = -1;

				loop{
					try {
						GRVStartTime = now();
						self->transactionFailureMetric->startLatency = -1;

						Version v = wait(self->inconsistentReads ? getInconsistentReadVersion(cx) : tr.getReadVersion());
						if(self->inconsistentReads) tr.setVersion(v);

						double grvLatency = now() - GRVStartTime;
						self->transactionSuccessMetric->startLatency = grvLatency * 1e9;
						self->transactionFailureMetric->startLatency = grvLatency * 1e9;
						if( self->shouldRecord() )
							self->GRVLatencies.addSample(grvLatency);

						state double readStart = now();
						wait(self->readOp(&tr, keys, self, self->shouldRecord()));

						double readLatency = now() - readStart;
						if( self->shouldRecord() )
							self->fullReadLatencies.addSample(readLatency);

						if(!writes)
							break;

						if (self->adjacentWrites) {
							int64_t startKey = self->getRandomKey(self->nodeCount - writes);
							for (int op = 0; op < writes; op++)
								tr.set(self->keyForIndex(startKey+op, false), values[op]);
						}
						else {
							for (int op = 0; op < writes; op++)
								tr.set(self->keyForIndex(self->getRandomKey(self->nodeCount), false), values[op]);
						}
						for (int op = 0; op < extra_read_conflict_ranges; op++)
							tr.addReadConflictRange(extra_ranges[op]);
						for (int op = 0; op < extra_write_conflict_ranges; op++)
							tr.addWriteConflictRange(extra_ranges[op + extra_read_conflict_ranges]);

						state double commitStart = now();
						wait(tr.commit());

						double commitLatency = now() - commitStart;
						self->transactionSuccessMetric->commitLatency = commitLatency * 1e9;
						if( self->shouldRecord() )
							self->commitLatencies.addSample(commitLatency);

						break;
					}
					catch(Error& e) {
						self->transactionFailureMetric->errorCode = e.code();
						self->transactionFailureMetric->log();

						wait(tr.onError(e));

						++self->transactionSuccessMetric->retries;
						++self->totalRetriesMetric;

						if(self->shouldRecord())
							++self->retries;
					}
				}

				if(debugID != UID())
					g_traceBatch.addEvent("TransactionDebug", debugID.first(), "ReadWrite.randomReadWriteClient.After");

				tr = Trans();

				double transactionLatency = now() - tstart;
				self->transactionSuccessMetric->totalLatency = transactionLatency * 1e9;
				self->transactionSuccessMetric->log();

				if(self->shouldRecord()) {
					if(aTransaction)
						++self->aTransactions;
					else
						++self->bTransactions;

					self->latencies.addSample(transactionLatency);
				}
			}
		}
	}
};

WorkloadFactory<ReadWriteWorkload> ReadWriteWorkloadFactory("ReadWrite");

