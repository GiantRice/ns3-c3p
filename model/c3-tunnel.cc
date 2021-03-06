#include "c3-tunnel.h"

#include "ns3/log.h"
#include "ns3/build-profile.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("C3Tunnel");

namespace c3p {

NS_OBJECT_ENSURE_REGISTERED (C3Tunnel);

TypeId
C3Tunnel::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::c3p::C3Tunnel")
      .SetParent<Object> ()
      .SetGroupName ("C3p")
      .AddAttribute ("Gamma",
                     "0 < Gamma < 1 is the weight given to new samples"
                     " against the past in the estimation of alpha.",
                     DoubleValue (1.0 / 16),
                     MakeDoubleAccessor (&C3Tunnel::m_g),
                     MakeDoubleChecker<double> (0.0, 1.0))
      .AddAttribute ("Interval",
                     "Interval to execute tunnel update.",
                     TimeValue (Time ("100us")),
                     MakeTimeAccessor (&C3Tunnel::m_interval),
                     MakeTimeChecker (Time (0)))
      .AddAttribute ("MaxRate",
                     "Max data rate of current tunnel.",
                     DataRateValue (DataRate ("1000Mbps")),
                     MakeDataRateAccessor (&C3Tunnel::m_rateMax),
                     MakeDataRateChecker ())
      .AddAttribute ("MinRate",
                     "Min data rate of current tunnel.",
                     DataRateValue (DataRate ("1Mbps")),
                     MakeDataRateAccessor (&C3Tunnel::m_rateMin),
                     MakeDataRateChecker ())
      .AddAttribute ("RateThresh",
                     "Rate thresh to determin when to start congestion avoidance.",
                     DataRateValue (DataRate ("500Mbps")),
                     MakeDataRateAccessor (&C3Tunnel::m_rateThresh),
                     MakeDataRateChecker ())
      .AddTraceSource ("Alpha",
                       "an estimate of the fraction of packets that are marked",
                       MakeTraceSourceAccessor (&C3Tunnel::m_alpha),
                       "ns3::TracedValueCallback::Double")
      .AddTraceSource ("Weight",
                       "Weight allocated to the tunnel.",
                       MakeTraceSourceAccessor (&C3Tunnel::m_weight),
                       "ns3::TracedValueCallback::Double")
      .AddTraceSource ("WeightRequest",
                       "Weight required by the tunnel.",
                       MakeTraceSourceAccessor (&C3Tunnel::m_weightRequest),
                       "ns3::TracedValueCallback::Double")
  ;
  return tid;
}

C3Tunnel::C3Tunnel (uint32_t tenantId, C3Type type,
                    const Ipv4Address &src, const Ipv4Address &dst)
  : m_src (src),
    m_dst (dst),
    m_alpha (1.0),
    m_g (0.625),
    m_weight (0.0),
    m_weightRequest (0.0),
    m_rate (0),
    m_sentBytes (0),
    m_timer (Timer::CANCEL_ON_DESTROY)
{
  NS_LOG_FUNCTION (this);
  m_ecnRecorder = C3EcnRecorder::CreateEcnRecorder (tenantId, type, src, dst);
  Simulator::ScheduleNow (&C3Tunnel::Initialize, this);
}

C3Tunnel::~C3Tunnel ()
{
  NS_LOG_FUNCTION (this);
}

void
C3Tunnel::SetRoute (Ptr<Ipv4Route> route)
{
  NS_LOG_FUNCTION (this << route);
  m_route = route;
}

void
C3Tunnel::SetForwardTarget (ForwardTargetCallback cb)
{
  NS_LOG_FUNCTION (this);
  m_forwardTarget = cb;
}

void
C3Tunnel::Update (void)
{
  NS_LOG_FUNCTION (this);
  UpdateInfo ();
  UpdateRate ();
  ScheduleFlow ();
  // clear stat in last timeslice
  m_ecnRecorder->Reset ();
  m_sentBytes = 0;
  // schedule next event
  m_timer.Schedule (m_interval);
}

double
C3Tunnel::GetWeightRequest (void) const
{
  return m_weightRequest;
}

void
C3Tunnel::SetWeight (double weight)
{
  NS_LOG_FUNCTION (this << weight);
  m_weight = weight;
}

void
C3Tunnel::SetRateThresh (DataRate rate)
{
  NS_LOG_FUNCTION (this << rate);
  m_rateThresh = rate;
}

void
C3Tunnel::DoInitialize (void)
{
  NS_LOG_FUNCTION (this);
  // initialize timer
  m_timer.SetFunction (&C3Tunnel::Update, this);
  // set a proper interval to call the first update
  m_timer.Schedule (m_interval);
  Object::DoInitialize ();
}

void
C3Tunnel::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_timer.Cancel ();
  m_ecnRecorder = 0;
  m_forwardTarget.Nullify ();
  m_route = 0;
  m_flowList.clear ();
  Object::DoDispose ();
}

void
C3Tunnel::Forward (Ptr<Packet> packet,  uint8_t protocol)
{
  NS_LOG_FUNCTION (this << packet << (uint32_t)protocol);
  // record bytes sent
  m_sentBytes += packet->GetSize ();
  m_forwardTarget (packet, m_src, m_dst, protocol, m_route);
}

DataRate
C3Tunnel::GetRate (void) const
{
  return m_rate;
}

void
C3Tunnel::UpdateInfo (void)
{
  NS_LOG_FUNCTION (this);

  // update alpha
  m_alpha = (1 - m_g) * m_alpha + m_g * m_ecnRecorder->GetMarkedRatio ();

  double weightRequest = 0;
  for (auto it = m_flowList.begin (); it != m_flowList.end (); ++it)
    {
      Ptr<C3Flow> flow = it->second;
      if (!flow->IsFinished ())
        {
          flow->UpdateInfo ();
          weightRequest += flow->GetWeight ();
        }
    }
  m_weightRequest = weightRequest;
}

void
C3Tunnel::UpdateRate (void)
{
  NS_LOG_FUNCTION (this);
  DataRate rate;
  // calculate last rate (in bps)
  double prevRate = m_sentBytes * 8 / m_interval.GetSeconds ();
  if (m_ecnRecorder->GetMarkedBytes ())
    {
      NS_LOG_DEBUG ("Congestion detected, decrease tunnel rate.");
      ///\todo change congestion operations
      // rate = DataRate ((1 - m_alpha / 2) * m_rate.GetBitRate ());
      // rate = DataRate ((1 - std::pow (m_alpha.Get (), m_weight) / 2) * m_rate.GetBitRate ());
      rate = DataRate ((1 - m_alpha / 2) * prevRate);
      m_rateThresh = rate;
    }
  else
    {
      NS_LOG_DEBUG ("No congestion, increase tunnel rate.");
      if (m_rate < m_rateThresh)
        {
          NS_LOG_LOGIC ("Slow start like behavior.");
          rate = DataRate ((1 + m_weight) * prevRate);
        }
      else
        {
          NS_LOG_LOGIC ("Congestion avoidance like behavior.");
          rate = DataRate (prevRate + m_weight * DataRate ("10Mbps").GetBitRate ());
        }
    }
  m_rate = std::max (std::min (rate, m_rateMax), m_rateMin);
}

} //namespace c3p
} //namespace ns3
